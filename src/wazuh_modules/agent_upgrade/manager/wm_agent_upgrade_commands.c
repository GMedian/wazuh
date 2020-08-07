/*
 * Wazuh Module for Agent Upgrading
 * Copyright (C) 2015-2020, Wazuh Inc.
 * July 3, 2020.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */
#include "wazuh_modules/wmodules.h"
#include "wm_agent_upgrade_manager.h"
#include "wm_agent_upgrade_parsing.h"
#include "wm_agent_upgrade_tasks.h"
#include "wazuh_db/wdb.h"
#include "os_net/os_net.h"

/**
 * Analyze agent information and returns a JSON to be sent to the task manager
 * @param agent_id id of the agent
 * @param agent_task structure where the information of the agent will be stored
 * @param error_code variable to modify in case of failure
 * @return JSON task if success, NULL otherwise
 * */
static cJSON* wm_agent_upgrade_analyze_agent(int agent_id, wm_agent_task *agent_task, wm_upgrade_error_code *error_code);

/**
 * Validate the information of the agent and the task
 * @param agent_task structure with the information to be validated
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_NOT_MINIMAL_VERSION_SUPPORTED
 * @retval WM_UPGRADE_VERSION_SAME_MANAGER
 * @retval WM_UPGRADE_NEW_VERSION_LEES_OR_EQUAL_THAT_CURRENT
 * @retval WM_UPGRADE_NEW_VERSION_GREATER_MASTER)
 * @retval WM_UPGRADE_AGENT_IS_NOT_ACTIVE
 * @retval WM_UPGRADE_INVALID_ACTION_FOR_MANAGER
 * @retval WM_UPGRADE_GLOBAL_DB_FAILURE
 * */
static int wm_agent_upgrade_validate_agent_task(const wm_agent_task *agent_task);

/**
 * Start the upgrade procedure for the agents
 * @param json_response cJSON array where the responses for each agent will be stored
 * @param task_module_request cJSON array with the agents to be upgraded
 * */
static void wm_agent_upgrade_start_upgrades(cJSON *json_response, const cJSON* task_module_request);

/**
 * Send WPK file to agent and verify SHA1
 * @param agent_task structure with the information of the agent and the WPK
 * @return return_code
 * @retval WM_UPGRADE_SUCCESS
 * @retval WM_UPGRADE_WPK_SENDING_ERROR
 * @retval WM_UPGRADE_WPK_SHA1_DOES_NOT_MATCH
 * @retval WM_UPGRADE_WPK_FILE_DOES_NOT_EXIST
 * */
static int wm_agent_upgrade_send_wpk_to_agent(wm_agent_task *agent_task);

/**
 * Send a command to the agent and return the response
 * @param command request command to agent
 * @return response from agent
 * */
static char* wm_agent_upgrade_send_command_to_agent(const char *command);

/**
 * Send a single message to the task module and returns a response
 * @param command wm_upgrade_command that will be used to generate the message
 * @param agent_id id of the agent
 * @param status in case the comand is and upgdate of statuos
 * @return cJSON with the response from the task manager
 * */
static cJSON* wm_agent_upgrade_send_single_task(wm_upgrade_command command, int agent_id, const char* status_task);


char* wm_agent_upgrade_process_upgrade_command(const int* agent_ids, wm_upgrade_task* task) {
    char* response = NULL;
    int agent = 0;
    int agent_id = 0;
    cJSON* json_response = cJSON_CreateArray();
    cJSON *json_task_module_request = cJSON_CreateArray();

    while (agent_id = agent_ids[agent++], agent_id != OS_INVALID) {
        wm_upgrade_error_code error_code = WM_UPGRADE_SUCCESS;
        cJSON *task_request = NULL;
        wm_agent_task *agent_task = NULL;
        wm_upgrade_task *upgrade_task = NULL;

        agent_task = wm_agent_upgrade_init_agent_task();

        // Task information
        upgrade_task = wm_agent_upgrade_init_upgrade_task();
        w_strdup(task->wpk_repository, upgrade_task->wpk_repository);
        w_strdup(task->custom_version, upgrade_task->custom_version);
        upgrade_task->use_http = task->use_http;
        upgrade_task->force_upgrade = task->force_upgrade;

        agent_task->task_info = wm_agent_upgrade_init_task_info();
        agent_task->task_info->command = WM_UPGRADE_UPGRADE;
        agent_task->task_info->task = upgrade_task;

        if (task_request = wm_agent_upgrade_analyze_agent(agent_id, agent_task, &error_code), task_request) {
            cJSON_AddItemToArray(json_task_module_request, task_request);
        } else {
            cJSON *error_message = wm_agent_upgrade_parse_response_message(error_code, upgrade_error_codes[error_code], &agent_id, NULL, NULL);
            cJSON_AddItemToArray(json_response, error_message);
            wm_agent_upgrade_free_agent_task(agent_task);
        }
    }

    wm_agent_upgrade_start_upgrades(json_response, json_task_module_request);

    response = cJSON_PrintUnformatted(json_response);

    cJSON_Delete(json_task_module_request);
    cJSON_Delete(json_response);

    return response;
}

char* wm_agent_upgrade_process_upgrade_custom_command(const int* agent_ids, wm_upgrade_custom_task* task) {
    char* response = NULL;
    int agent = 0;
    int agent_id = 0;
    cJSON* json_response = cJSON_CreateArray();
    cJSON *json_task_module_request = cJSON_CreateArray();

    while (agent_id = agent_ids[agent++], agent_id != OS_INVALID) {
        wm_upgrade_error_code error_code = WM_UPGRADE_SUCCESS;
        cJSON *task_request = NULL;
        wm_agent_task *agent_task = NULL;
        wm_upgrade_custom_task *upgrade_custom_task = NULL;

        agent_task = wm_agent_upgrade_init_agent_task();

        // Task information
        upgrade_custom_task = wm_agent_upgrade_init_upgrade_custom_task();
        w_strdup(task->custom_file_path, upgrade_custom_task->custom_file_path);
        w_strdup(task->custom_installer, upgrade_custom_task->custom_installer);

        agent_task->task_info = wm_agent_upgrade_init_task_info();
        agent_task->task_info->command = WM_UPGRADE_UPGRADE_CUSTOM;
        agent_task->task_info->task = upgrade_custom_task;

        if (task_request = wm_agent_upgrade_analyze_agent(agent_id, agent_task, &error_code), task_request) {
            cJSON_AddItemToArray(json_task_module_request, task_request);
        } else {
            cJSON *error_message = wm_agent_upgrade_parse_response_message(error_code, upgrade_error_codes[error_code], &agent_id, NULL, NULL);
            cJSON_AddItemToArray(json_response, error_message);
            wm_agent_upgrade_free_agent_task(agent_task);
        }
    }

    wm_agent_upgrade_start_upgrades(json_response, json_task_module_request);

    response = cJSON_PrintUnformatted(json_response);

    cJSON_Delete(json_task_module_request);
    cJSON_Delete(json_response);

    return response;
}

static cJSON* wm_agent_upgrade_analyze_agent(int agent_id, wm_agent_task *agent_task, wm_upgrade_error_code *error_code) {
    cJSON *task_request = NULL;

    // Agent information
    agent_task->agent_info = wm_agent_upgrade_init_agent_info();
    agent_task->agent_info->agent_id = agent_id;

    if (!wdb_agent_info(agent_id,
                        &agent_task->agent_info->platform,
                        &agent_task->agent_info->major_version,
                        &agent_task->agent_info->minor_version,
                        &agent_task->agent_info->architecture,
                        &agent_task->agent_info->wazuh_version,
                        &agent_task->agent_info->last_keep_alive)) {

        // Validate agent and task information
        *error_code = wm_agent_upgrade_validate_agent_task(agent_task);

        if (*error_code == WM_UPGRADE_SUCCESS) {
            // Save task entry for agent
            int result = wm_agent_upgrade_create_task_entry(agent_id, agent_task);

            if (result == OSHASH_SUCCESS) {
                task_request = wm_agent_upgrade_parse_task_module_request(agent_task->task_info->command, agent_id, NULL);
            } else if (result == OSHASH_DUPLICATED) {
                *error_code = WM_UPGRADE_UPGRADE_ALREADY_IN_PROGRESS;
            } else {
                *error_code = WM_UPGRADE_UNKNOWN_ERROR;
            }
        }
    } else {
        *error_code = WM_UPGRADE_GLOBAL_DB_FAILURE;
    }

    return task_request;
}

static int wm_agent_upgrade_validate_agent_task(const wm_agent_task *agent_task) {
    int validate_result = WM_UPGRADE_SUCCESS;

    // Validate agent id
    validate_result = wm_agent_upgrade_validate_id(agent_task->agent_info->agent_id);

    if (validate_result != WM_UPGRADE_SUCCESS) {
        return validate_result;
    }

    // Validate agent status
    validate_result = wm_agent_upgrade_validate_status(agent_task->agent_info->last_keep_alive);

    if (validate_result != WM_UPGRADE_SUCCESS) {
        return validate_result;
    }


    // TODO: Check if there isn't already a task for this agent with status UPDATING


    // Validate Wazuh version to upgrade
    validate_result = wm_agent_upgrade_validate_version(agent_task->agent_info, agent_task->task_info->task, agent_task->task_info->command);

    if (validate_result != WM_UPGRADE_SUCCESS) {
        return validate_result;
    }

    // Validate WPK file
    if (WM_UPGRADE_UPGRADE == agent_task->task_info->command) {
        validate_result = wm_agent_upgrade_validate_wpk((wm_upgrade_task *)agent_task->task_info->task);
    } else {
        validate_result = wm_agent_upgrade_validate_wpk_custom((wm_upgrade_custom_task *)agent_task->task_info->task);
    }

    return validate_result;
}

static void wm_agent_upgrade_start_upgrades(cJSON *json_response, const cJSON* task_module_request) {
    unsigned int index = 0;
    OSHashNode *node = NULL;
    char *agent_key = NULL;
    wm_agent_task *agent_task = NULL;

    // Send request to task module and store task ids
    if (!wm_agent_upgrade_parse_task_module_task_ids(json_response, task_module_request)) {
        node = wm_agent_upgrade_get_first_node(&index);

        while (node) {
            agent_key = node->key;
            agent_task = (wm_agent_task *)node->data;
            node = wm_agent_upgrade_get_next_node(&index, node);


            // TODO: Send WPK to agent and update task to UPDATING/ERROR


            wm_agent_upgrade_remove_entry(atoi(agent_key));
        }
    } else {
        mtwarn(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_NO_AGENTS_TO_UPGRADE);
    }
}

char* wm_agent_upgrade_process_agent_result_command(const int* agent_ids, wm_upgrade_agent_status_task* task) {
    // Only one id of agent will reach at a time
    int agent_id = agent_ids[0];
    mtinfo(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_ACK_RECEIVED, agent_id, task->error_code, task->message);

    // Send task update to task manager and bring back the response
    cJSON *response = wm_agent_upgrade_send_single_task(WM_UPGRADE_AGENT_STATUS, agent_id, task->status);

    if (wm_agent_upgrade_validate_task_update_message(response)) {
        // If status update is successful, tell agent to erase results file
        char *buffer = NULL;
        os_calloc(OS_MAXSTR, sizeof(char), buffer);
        sprintf(buffer, "%03d com clear_upgrade_result -1", agent_id);
        char *agent_response = wm_agent_upgrade_send_command_to_agent(buffer); 
        char *data = NULL;
        char *error = NULL;
        if (wm_agent_upgrade_parse_agent_response(agent_response, &data, &error) == OS_SUCCESS) {
            mtdebug1(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_UPGRADE_FILE_AGENT);
        } else {
            mterror(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_RESULT_FILE_ERROR, data);
        }
        os_free(buffer);
        os_free(agent_response);
    }
    
    char *message = cJSON_PrintUnformatted(response);
    
    cJSON_Delete(response);
    return message;
}

static char* wm_agent_upgrade_send_command_to_agent(const char *command) {
    char *response = NULL;
    int length = 0;

    const char *path = isChroot() ? REMOTE_REQ_SOCK : DEFAULTDIR REMOTE_REQ_SOCK;

    int sock = OS_ConnectUnixDomain(path, SOCK_STREAM, OS_MAXSTR);

    if (sock == OS_SOCKTERR) {
        mterror(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_UNREACHEABLE_REQUEST, path);
    } else {
        mtdebug1(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_REQUEST_SEND_MESSAGE, command);

        OS_SendSecureTCP(sock, strlen(command), command);
        os_calloc(OS_MAXSTR, sizeof(char), response);

        switch (length = OS_RecvSecureTCP(sock, response, OS_MAXSTR), length) {
            case OS_SOCKTERR:
                mterror(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_SOCKTERR_ERROR);
                break;
            case -1:
                mterror(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_RECV_ERROR, strerror(errno));
                break;
            default:
                if (!response) {
                    mterror(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_EMPTY_AGENT_RESPONSE);
                } else {
                    mtdebug1(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_REQUEST_RECEIVE_MESSAGE, response);
                }
                break;
        }
    }

    return response;
}

static cJSON* wm_agent_upgrade_send_single_task(wm_upgrade_command command, int agent_id, const char* status_task) {
    cJSON *response = NULL;
    cJSON *message_object = wm_agent_upgrade_parse_task_module_request(command, agent_id, status_task);
    cJSON *message_array = cJSON_CreateArray();
    cJSON_AddItemToArray(message_array, message_object);

    cJSON* task_module_response = wm_agent_upgrade_send_tasks_information(message_array);

    if (task_module_response && (task_module_response->type == cJSON_Array) && (cJSON_GetArraySize(task_module_response) == 1)) {
        response = cJSON_DetachItemFromArray(task_module_response, 0);
        cJSON_Delete(task_module_response);
    } else {
        mterror(WM_AGENT_UPGRADE_LOGTAG, WM_UPGRADE_INVALID_TASK_MAN_JSON);
        response = task_module_response;
    }
    cJSON_Delete(message_array);
    return response;
}