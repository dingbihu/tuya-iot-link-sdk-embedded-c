#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tuya_error_code.h"
#include "tuya_iot.h"
#include "tuya_log.h"
#include "tuya_url.h"

#include "system_interface.h"
#include "storage_interface.h"
#include "atop_base.h"
#include "atop_service.h"
#include "mqtt_bind.h"
#include "cJSON.h"

#define ACTIVATE_MAXLEN             (255)
#define SCHEMA_MAXLEN               (4096)
#define TOKEN_LEN_MIN               (8)
#define MQTT_NETCFG_TIMEOUT         (5000)
#define MQTT_RECV_TIMEOUT           (2000)
#define MAX_LENGTH_ACTIVATE_BUFFER  (1024*8)

extern const char tuya_rootCA_pem[];

typedef enum {
    STATE_IDLE,
    STATE_START,
    STATE_DATA_LOAD,
    STATE_TOKEN_PENDING,
    STATE_ACTIVATING,
    STATE_STARTUP_UPDATE,
    STATE_MQTT_CONNECT_START,
    STATE_MQTT_CONNECTING,
    STATE_MQTT_YIELD,
    STATE_RESTART,
    STATE_RESET,
    STATE_STOP,
    STATE_EXIT,

    STATE_OTA_MODE,
    STATE_OTA_MODE_START,
    STATE_OTA_MODE_RUNING,
    STATE_OTA_MODE_EXIT,

} tuya_run_state_t;


/* -------------------------------------------------------------------------- */
/*                          Internal utils functions                          */
/* -------------------------------------------------------------------------- */

static int iot_dispatch_event(tuya_iot_client_t* client)
{
    if (client->config.event_handler) {
        client->config.event_handler(client, &client->event);
    }
    return OPRT_OK;
}

/* -------------------------------------------------------------------------- */
/*                            Activate data process                           */
/* -------------------------------------------------------------------------- */

static int activate_json_string_parse(const char* str, activated_params_t* out)
{
    cJSON* root = cJSON_Parse(str);
    if (NULL == root) {
        cJSON_Delete(root);
        return OPRT_CJSON_PARSE_ERR;
    }

    if (cJSON_GetObjectItem(root, "devId") == NULL || \
        cJSON_GetObjectItem(root, "secKey") == NULL || \
        cJSON_GetObjectItem(root, "localKey") == NULL || \
        cJSON_GetObjectItem(root, "schemaId") == NULL) {
        cJSON_Delete(root);
        return OPRT_CJSON_GET_ERR;
    }
    
    strcpy(out->devid, cJSON_GetObjectItem(root, "devId")->valuestring);
    strcpy(out->seckey, cJSON_GetObjectItem(root, "secKey")->valuestring);
    strcpy(out->localkey, cJSON_GetObjectItem(root, "localKey")->valuestring);
    strcpy(out->schemaId, cJSON_GetObjectItem(root, "schemaId")->valuestring);
    cJSON_Delete(root);
    return OPRT_OK;
}

static int activated_data_read(const char* storage_key, activated_params_t* out)
{
    int rt = OPRT_OK;
    size_t readlen = ACTIVATE_MAXLEN;
    char* readbuf = system_calloc(sizeof(char), ACTIVATE_MAXLEN);
    if (NULL == readbuf) {
        TY_LOGE("activate_string malloc fail.");
        return rt;
    }

    /* Try read activate config data */
    rt = local_storage_get((const char*)storage_key, (uint8_t*)readbuf, &readlen);
    if (OPRT_OK != rt) {
        TY_LOGW("activate config not found:%d", rt);
        system_free(readbuf);
        return rt;
    }

    /* Parse activate json string */
    rt = activate_json_string_parse((const char*)readbuf, out);
    system_free(readbuf);
    if (OPRT_OK != rt) {
        TY_LOGE("activate_json_string_parse fail:%d", rt);
        return rt;
    }

    /* Dump info */
    TY_LOGV("devId: %s", out->devid);
    TY_LOGV("secKey: %s", out->seckey);
    TY_LOGV("localKey: %s", out->localkey);

    return rt;
}

static int activate_response_parse(atop_base_response_t* response)
{
    if (response->success != true || response->result == NULL) {
        return OPRT_INVALID_PARM;
    }

    int ret = OPRT_OK;
    tuya_iot_client_t* client = (tuya_iot_client_t*)response->user_data;
    cJSON* result_root = response->result;

    if (!cJSON_HasObjectItem(result_root, "schema") || 
        !cJSON_HasObjectItem(result_root, "schemaId")) {
        TY_LOGE("not found schema");
        cJSON_Delete(result_root);
        return OPRT_CJSON_GET_ERR;
    }

    // cJSON object to string save
    char* schemaId = cJSON_GetObjectItem(result_root, "schemaId")->valuestring;
    cJSON* schema_obj = cJSON_DetachItemFromObject(result_root, "schema");
    ret = local_storage_set(schemaId, (const uint8_t*)schema_obj->valuestring, strlen(schema_obj->valuestring));
    cJSON_Delete(schema_obj);
    if (ret != OPRT_OK) {
        TY_LOGE("activate data save error:%d", ret);
        return OPRT_KVS_WR_FAIL;
    }

    // activate info save
    char* result_string = cJSON_PrintUnformatted(result_root);
    const char* activate_data_key = client->config.uuid;
    TY_LOGD("result len %d :%s", (int)strlen(result_string), result_string);
    ret = local_storage_set(activate_data_key, (const uint8_t*)result_string, strlen(result_string));
    system_free(result_string);
    if (ret != OPRT_OK) {
        TY_LOGE("activate data save error:%d", ret);
        return OPRT_KVS_WR_FAIL;
    }

    if(cJSON_GetObjectItem(result_root,"resetFactory") != NULL) {
        BOOL_T cloud_reset_factory = (cJSON_GetObjectItem(result_root,"resetFactory")->type == cJSON_True)? TRUE:FALSE;
        TY_LOGD("cloud_reset:%d", cloud_reset_factory);
        //目前只有判断APP恢复出厂模式,但是本地简单移除配网信息,那么告知用户
        if(cloud_reset_factory == TRUE) {
            TY_LOGD("remote is reset factory and local is not,reset factory again.");
            client->event.data = (void*)GW_RESET_DATA_FACTORY;
            client->event.id = TUYA_EVENT_RESET;
            iot_dispatch_event(client);
        }
    }

    // netfcg_state switch to complete;
    return OPRT_OK;
}

static int client_activate_process(tuya_iot_client_t* client, const char* token)
{
    /* acvitive request instantiate construct */
    device_activite_params_t activite_request = {
        .token = (const char*)token,
        .product_key = client->config.productkey,
        .uuid = client->config.uuid,
        .authkey = client->config.authkey,
        .sw_ver = client->config.software_ver,
        .bv = BS_VERSION,
        .pv = PV_VERSION,
        .buflen_custom = MAX_LENGTH_ACTIVATE_BUFFER,
        .user_data = client
    };

    /* atop response instantiate construct */
    atop_base_response_t response = {0};

    /* start activate request send */
    int rt = tuya_device_activate_request(&activite_request, &response);
    if (OPRT_OK != rt) {
        TY_LOGE("http active error:%d", rt);
        client->state = STATE_RESTART;
        return rt;
    }

    /* Parse activate response json data */
    rt = activate_response_parse(&response);

    /* relese response object */
    atop_base_response_free(&response);

    if (OPRT_OK != rt) {
        TY_LOGE("activate_response_parse error:%d", rt);
        return rt;
    }

    return OPRT_OK;
}

/* -------------------------------------------------------------------------- */
/*                         Tuya MQTT service callback                         */
/* -------------------------------------------------------------------------- */

static void mqtt_service_dp_receive_on(tuya_mqtt_event_t* ev)
{
    tuya_iot_client_t* client = ev->user_data;
    cJSON* data = (cJSON*)(ev->data);
    if (NULL == cJSON_GetObjectItem(data, "dps")) {
        TY_LOGE("not found dps");
        return;
    }

    /* Get dps string json */
    char* dps_string = cJSON_PrintUnformatted(cJSON_GetObjectItem(data, "dps"));
	TY_LOGV("dps: \r\n%s", dps_string);

    /* Send DP string format event*/
    client->event.id = TUYA_EVENT_DP_RECEIVE;
    client->event.data = dps_string;
    client->event.length = strlen(dps_string);
    iot_dispatch_event(client);
    system_free(dps_string);

    /* Send DP cJSON format event*/
    client->event.id = TUYA_EVENT_DP_RECEIVE_CJSON;
    client->event.data = cJSON_GetObjectItem(data, "dps");
    client->event.length = 0;
    iot_dispatch_event(client);
}

static void mqtt_service_reset_cmd_on(tuya_mqtt_event_t* ev)
{
    tuya_iot_client_t* client = ev->user_data;
    cJSON* data = (cJSON*)(ev->data);

    if (NULL == cJSON_GetObjectItem(data, "gwId")) {
        TY_LOGE("not found gwId");
    }

    TY_LOGW("Reset id:%s", cJSON_GetObjectItem(data, "gwId")->valuestring);

    /* DP event send */
    client->event.id = TUYA_EVENT_RESET;

    if (cJSON_GetObjectItem(data, "type") && \
        strcmp(cJSON_GetObjectItem(data, "type")->valuestring, "reset_factory") == 0)  {
        TY_LOGD("cmd is reset factory, ungister");
        client->event.data = (void*)GW_REMOTE_RESET_FACTORY;
    } else {
        TY_LOGD("unactive");
        client->event.data = (void*)GW_REMOTE_UNACTIVE;
    }
    iot_dispatch_event(client);

    client->state = STATE_RESET;
    TY_LOGI("STATE_RESET...");
}

/* -------------------------------------------------------------------------- */
/*                       Internal machine state process                       */
/* -------------------------------------------------------------------------- */

static int run_state_startup_update(tuya_iot_client_t* client)
{
    int rt = OPRT_OK;

    atop_base_response_t response = {0};

    rt = atop_service_dynamic_cfg_get_v20(  client->activate.devid, 
                                            client->activate.seckey, 
                                            HTTP_DYNAMIC_CFG_ALL, 
                                            &response);
    if (rt != OPRT_OK) {
        TY_LOGE("dynamic_cfg_get error:%d", rt);
        return rt;
    }

    /* TODO result process*/
    atop_base_response_free(&response);

    return rt;
}

static int run_state_mqtt_connect_start(tuya_iot_client_t* client)
{
    int rt = OPRT_OK;

    /* mqtt init */
    rt = tuya_mqtt_init(&client->mqctx, &(const tuya_mqtt_config_t){
        .rootCA = tuya_rootCA_pem,
        .host = tuya_mqtt_server_host_get(),
        .port = tuya_mqtt_server_port_get(),
        .devid = client->activate.devid,
        .seckey = client->activate.seckey,
        .localkey = client->activate.localkey,
        .timeout = MQTT_RECV_TIMEOUT,
    });
    if (OPRT_OK != rt) {
        TY_LOGE("tuya mqtt init error:%d", rt);
        return rt;
    }

    rt = tuya_mqtt_start(&client->mqctx);
    if (OPRT_OK != rt) {
        TY_LOGE("tuya mqtt start error:%d", rt);
        tuya_mqtt_destory(&client->mqctx);
        client->state = STATE_RESTART;
        return rt;
    }

    /* callback register */
    tuya_mqtt_protocol_register(&client->mqctx, PRO_CMD, mqtt_service_dp_receive_on, client);
    tuya_mqtt_protocol_register(&client->mqctx, PRO_GW_RESET, mqtt_service_reset_cmd_on, client);

    return rt;
}

static int run_state_restart(tuya_iot_client_t* client)
{
    TY_LOGW("CLIENT RESTART!");
    return OPRT_OK;
}

static int run_state_reset(tuya_iot_client_t* client)
{
    TY_LOGW("CLIENT RESET...");

    /* Stop MQTT service */
    tuya_mqtt_stop(&client->mqctx);

    /* Clean client local data */
    local_storage_del((const char*)(client->activate.schemaId));
    local_storage_del((const char*)(client->config.uuid));

    return OPRT_OK;
}

/* -------------------------------------------------------------------------- */
/*                                Tuya IoT API                                */
/* -------------------------------------------------------------------------- */

int tuya_iot_init(tuya_iot_client_t* client, const tuya_iot_config_t* config)
{
    int ret = OPRT_OK;
    TY_LOGI("tuya_iot_init");
    if (NULL == client || NULL == config) {
        return OPRT_INVALID_PARM;
    }

    /* config params check */
    if (NULL == config->productkey || NULL == config->uuid || NULL == config->authkey) {
        return OPRT_INVALID_PARM;
    }

    /* Initialize all tuya_iot_client_t structs to 0. */
    memset(client, 0, sizeof(tuya_iot_client_t));

    /* Save the client config */
    client->config = *config;

    /* Config param dump */
    TY_LOGD("software_ver:%s", client->config.software_ver);
    TY_LOGD("productkey:%s", client->config.productkey);
    TY_LOGD("uuid:%s", client->config.uuid);
    TY_LOGD("authkey:%s", client->config.authkey);

    /* cJSON init */
    cJSON_Hooks hooks = {
        .malloc_fn = system_malloc,
        .free_fn = system_free
    };
    cJSON_InitHooks(&hooks);

    client->state = STATE_IDLE;
    return ret;
}

int tuya_iot_start(tuya_iot_client_t *client)
{
    client->state = STATE_START;
    return OPRT_OK;
}

int tuya_iot_stop(tuya_iot_client_t *client)
{
    client->state = STATE_STOP;
    return OPRT_OK;
}

int tuya_iot_reset(tuya_iot_client_t *client)
{
    int ret = OPRT_OK;
    if (tuya_iot_activated(client)) {
        atop_base_response_t response = {0};
        ret = atop_service_client_reset(
                client->activate.devid, 
                client->activate.seckey, 
                &response);
        atop_base_response_free(&response);
    }

    client->event.id = TUYA_EVENT_RESET;
    client->event.data = (void*)GW_LOCAL_RESET_FACTORY;
    iot_dispatch_event(client);
    client->state = STATE_RESET;
    return ret;
}

int tuya_iot_destroy(tuya_iot_client_t* client)
{
    return OPRT_OK;
}

int tuya_iot_yield(tuya_iot_client_t* client)
{
    int ret = OPRT_OK;
    switch (client->state) {
    case STATE_MQTT_YIELD:
        break;

    case STATE_IDLE:
        system_sleep(500);
        break;

    case STATE_START:
        TY_LOGD("STATE_START");
        client->state = STATE_DATA_LOAD;
        break;

    case STATE_DATA_LOAD:
        /* Try to read the local activation data. 
         * If the reading is successful, the device has been activated. */
        if (activated_data_read(client->config.uuid, &client->activate) == OPRT_OK) {
            client->state = STATE_STARTUP_UPDATE;
            break;
        }

        /* If the reading fails, enter the pending activation mode. */
        TY_LOGI("Activation data read fail, go activation mode...");
        memset(client->token, 0, MAX_LENGTH_TOKEN);
        client->state = STATE_TOKEN_PENDING;
        break;

    case STATE_TOKEN_PENDING:
        /* If token_get port no preset, Use default mqtt bind mode */
        if (client->token_get == NULL) {
            tuya_iot_token_get_port_register(client, mqtt_bind_token_get);
        }

        /* Send Bind event to user program */
        client->event.id = TUYA_EVENT_BIND_START;
        iot_dispatch_event(client);

        if (client->token_get(&client->config, client->token) == OPRT_OK) {
            TY_LOGI("token on: %s", client->token);

            /* DP event send */
            client->event.id = TUYA_EVENT_BIND_TOKEN_ON;
            client->event.data = client->token;
            client->event.length = strlen(client->token);
            iot_dispatch_event(client);

            /* Take token go to activate */
            client->state = STATE_ACTIVATING;
        }
        break;

    case STATE_ACTIVATING:
        ret = client_activate_process(client, client->token);
        if (OPRT_OK == ret) {
            client->event.id = TUYA_EVENT_ACTIVATE_SUCCESSED;
            iot_dispatch_event(client);
            client->state = STATE_DATA_LOAD;
        }
        break;

    case STATE_STARTUP_UPDATE:
        if (run_state_startup_update(client) == OPRT_LINK_CORE_HTTP_GW_NOT_EXIST) {
            client->state = STATE_RESET;
            break;
        }
        client->state = STATE_MQTT_CONNECT_START;
        break;

    case STATE_MQTT_CONNECT_START:
        run_state_mqtt_connect_start(client);
        client->state = STATE_MQTT_CONNECTING;
        break;

    case STATE_MQTT_CONNECTING:
        if (tuya_mqtt_connected(&client->mqctx)) {
            TY_LOGI("Tuya MQTT connected.");

            /* DP event send */
            client->event.id = TUYA_EVENT_MQTT_CONNECTED;
            iot_dispatch_event(client);

            client->state = STATE_MQTT_YIELD;
        }
        break;

    case STATE_RESTART:
        run_state_restart(client);
        client->state = STATE_START;
        break;

    case STATE_RESET:
        run_state_reset(client);
        client->state = STATE_RESTART;
        break;

    case STATE_STOP:
        tuya_mqtt_stop(&client->mqctx);
        client->state = STATE_IDLE;
        break;

    default:
        break;
    }

    /* background processing */
    tuya_mqtt_loop(&client->mqctx);

    return ret;
}

bool tuya_iot_activated(tuya_iot_client_t* client)
{
    if (client->state == STATE_MQTT_YIELD) {
        return true;
    }
    return false;
}

int tuya_iot_dp_report_json(tuya_iot_client_t* client, const char* dps)
{
    if (client == NULL || dps == NULL) {
        TY_LOGE("param error");
        return OPRT_INVALID_PARM;
    }

    int rt = OPRT_OK;
    int printlen = 0;
    char* buffer = NULL;

    /* Package JSON format */
    {
        buffer = system_malloc(strlen(dps) + 64);
        if (NULL == buffer) {
            return OPRT_MALLOC_FAILED;
        }
        printlen = sprintf(buffer, "{\"devId\":\"%s\",\"dps\":%s}", client->activate.devid, dps);
    }

    /* Report buffer */
    rt = tuya_mqtt_report_data(&client->mqctx, PRO_DATA_PUSH, (uint8_t*)buffer, printlen);
    system_free(buffer);

    return rt;
}

void tuya_iot_token_get_port_register(tuya_iot_client_t* client, tuya_activate_token_get_t token_get_func)
{
    client->token_get = token_get_func;
}