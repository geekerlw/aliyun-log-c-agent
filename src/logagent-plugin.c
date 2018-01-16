/*
 * Copyright (c) 2018 Steven Lee <geekerlw@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <json.h>

#include "logagent.h"
#include "logagent-log.h"
#include "logagent-list.h"
#include "logagent-plugin.h"


static void logagent_plugin_list_add(struct list_head *plugin_list, const char *json)
{
	plugin_t *pdata = (plugin_t *) malloc(sizeof(plugin_t));
	if (!pdata)
		return;
	memset(pdata, 0, sizeof(pdata));

	memcpy(pdata->json, json, strlen(json) + 1);

	list_append(plugin_list, &pdata->list);

	return;
}

static void logagent_plugin_list_remove(struct list_head *pos)
{
	if (pos == pos->next)
		return;

	plugin_t *pdata = list_entry(pos, plugin_t, list);

	list_remove(pos);

	free(pdata);

	return;
}

static void logagent_plugin_list_init(struct list_head *plugin_list)
{
	list_init(plugin_list);
}

static void logagent_plugin_list_destroy(struct list_head *plugin_list)
{
	struct list_head *pos, *n;
	list_for_each_safe(pos, n, plugin_list) {
		plugin_t *node = list_entry(pos, plugin_t, list);
		list_remove(pos);
		free(node);
		pos = n;
	}

	return;
}

static void logagent_plugin_init(plugin_t *plugin)
{
	int ret = -1;
	
	plugin->context = (void *)&plugin->json;

	ret = plugin->init(plugin->context);
	
	if (ret != 0) {
		LOGAGENT_LOG_ERROR("[%s]->init() call error, return: %d\n", plugin->name, ret);
	}
	
	plugin->config = *plugin->context;

	return;
}

void logagent_plugin_init_all(struct list_head *plugin_list)
{
	plugin_t *plugin;
	list_for_each_entry(plugin, plugin_t, plugin_list, list) {
		logagent_plugin_init(plugin);
	}

	return;
}

static void logagent_plugin_exit(plugin_t *plugin)
{
	int ret = -1;
	plugin->context = (void *)&plugin->config;
	
	ret = plugin->exit(plugin->context);
	if (ret != 0) {
		LOGAGENT_LOG_ERROR("[%s]->exit() call error, return: %d\n", plugin->name, ret);
	}

	return;
}

void logagent_plugin_exit_all(struct list_head *plugin_list)
{
	plugin_t *plugin;
	list_for_each_entry(plugin, plugin_t, plugin_list, list) {
		logagent_plugin_exit(plugin);
	}
	
	return;
}

static void logagent_plugin_work(plugin_t *plugin, struct list_head *log_list)
{
	int ret = -1;
	
	ret = plugin->work(plugin->config, log_list);
	if (ret != 0) {
		LOGAGENT_LOG_ERROR("[%s]->work() call error, return: %d\n", plugin->name, ret);
	}

	return;
}

void logagent_plugin_work_all(struct list_head *plugin_list)
{
	plugin_t *plugin;
	struct list_head log_list;

	list_init(&log_list);

	list_for_each_entry(plugin, plugin_t, plugin_list, list) {
		logagent_plugin_work(plugin, &log_list);
	}
	
	return;
}

static void logagent_plugin_load(plugin_t *plugin)
{
	json_object *plugin_path_obj, *plugin_name_obj;
	json_bool ret;

	json_object *plugin_obj = json_tokener_parse(plugin->json);
	if (!plugin_obj) {
		LOGAGENT_LOG_FATAL("can't parse config json string: %s\n", plugin->json);
		return;
	}

	ret = json_object_object_get_ex(plugin_obj, "plugin_path", &plugin_path_obj);
	if (ret == false) {
		LOGAGENT_LOG_FATAL("can't get plugin path from json: %s\n", plugin->json);
		goto err_json_parse;
	}
	
	ret = json_object_object_get_ex(plugin_obj, "plugin_name", &plugin_name_obj);
	if (ret == false) {
		LOGAGENT_LOG_FATAL("can't get plugin name from json: %s\n", plugin->json);
		goto err_json_parse;
	}

	json_object_put(plugin_obj);

	sprintf(plugin->path, "%s", json_object_get_string(plugin_path_obj));
	sprintf(plugin->name, "liblogagent-plugin-%s.so", json_object_get_string(plugin_name_obj));

	int length = strlen(plugin->path);
	int offset = plugin->path[length - 1] == '/' ? length -1 : length; 
	memcpy(plugin->path + offset, plugin->name, sizeof(plugin->name));

	plugin->lib_handle = dlopen(plugin_lib_path, RTLD_LAZY);

	if ( plugin->lib_handle == NULL) {
		LOGAGENT_LOG_FATAL("failed to open dynamic library: %s\n", plugin->name);
		return;
	}

	plugin->init = dlsym(plugin->lib_handle, "logagent_plugin_init");
	plugin->work = dlsym(plugin->lib_handle, "logagent_plugin_work");
	plugin->exit = dlsym(plugin->lib_handle, "logagent_plugin_exit");

	return;

err_json_parse:
	json_object_put(plugin_obj);

	return;
}

static void logagent_plugin_load_all(struct list_head *plugin_list)
{
	plugin_t *plugin;
	list_for_each_entry(plugin, plugin_t, plugin_list, list) {
		logagent_plugin_load(plugin);
	}

	return;
}

static void logagent_plugin_unload(plugin_t *plugin)
{
	if (plugin->lib_handle) {
		dlclose(plugin->lib_handle);
	}

	return;
}

static void logagent_plugin_unload_all(struct list_head *plugin_list)
{
	plugin_t *plugin;
	list_for_each_entry(plugin, plugin_t, plugin_list, list) {
		logagent_plugin_load(plugin);
	}

	return;
}

void logagent_plugin_config_load(struct list_head *plugin_list, const char *json)
{
	struct json_object plugin_nums_obj;
	struct json_object plugin_obj;
	json_bool ret;
	
	logagent_plugin_list_init(plugin_list);

	struct json_object *pipeline_obj = json_tokener_parse(json);
	if (pipeline_obj == NULL) {
		LOGAGENT_LOG_FATAL("can't parse config json string: %s\n", json);
		return;
	}

	ret = json_object_object_get_ex(pipeline_obj, "plugin_nums", &plugin_nums_obj);
	if (ret ==  false) {
		LOGAGENT_LOG_FATAL("can't get plugin nums from json: %s\n", json);
		goto err_json_parse;
	}
	
	int plugin_nums = json_object_get_int(plugin_nums_obj);

	for (int i = 0; i < plugin_nums; i++) {
		char plugin_name[20] = { 0 };
		sprintf(plugin_name, "plugin@%d", i);

		ret = json_object_object_get_ex(pipeline_obj, plugin_name, &plugin_obj);
		if (ret == false) {
			LOGAGENT_LOG_ERROR("can't found %s in json config, please check\n", plugin_name);
		}else {
			logagent_plugin_list_add(plugin_list, json_object_to_json_string(plugin_obj));
		}
	}

	json_object_put(plugin);

	logagent_plugin_load_all(plugin_list);

	return;

err_json_parse:
	json_object_put(pipeline_obj);

	return;
}

/* plugin config unload */
void logagent_plugin_config_unload(struct list_head *plugin_list)
{
	logagent_plugin_unload_all(plugin_list);
	logagent_plugin_list_destroy(plugin_list);
}
