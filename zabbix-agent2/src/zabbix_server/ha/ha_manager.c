/*
** Zabbix
** Copyright (C) 2001-2021 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "db.h"
#include "log.h"
#include "zbxipcservice.h"
#include "zbxserialize.h"
#include "threads.h"
#include "zbxjson.h"
#include "../../libs/zbxalgo/vectorimpl.h"
#include "../../libs/zbxaudit/audit.h"
#include "../../libs/zbxaudit/audit_ha.h"
#include "../../libs/zbxaudit/audit_settings.h"
#include "zbxha.h"
#include "ha.h"

#define ZBX_HA_POLL_PERIOD	5

#define ZBX_HA_DEFAULT_FAILOVER_DELAY	SEC_PER_MIN

#define ZBX_HA_NODE_LOCK	1

static pid_t			ha_pid = ZBX_THREAD_ERROR;
static zbx_ipc_async_socket_t	ha_socket;

extern char	*CONFIG_HA_NODE_NAME;
extern char	*CONFIG_NODE_ADDRESS;

extern zbx_cuid_t	ha_sessionid;

#define ZBX_HA_IS_CLUSTER()	(NULL != CONFIG_HA_NODE_NAME && '\0' != *CONFIG_HA_NODE_NAME)

typedef struct
{
	zbx_cuid_t	ha_nodeid;

	/* HA status */
	int		ha_status;

	/* database connection status */
	int		db_status;

	/* timestamp in database time */
	int		db_time;

	int		failover_delay;

	/* last access time of active node */
	int		lastaccess_active;

	/* number of ticks active node has not been updated its lastaccess */
	int		offline_ticks_active;

	/* 0 if auditlog is disabled */
	int		auditlog;

	const char	*name;
	char		*error;
}
zbx_ha_info_t;

ZBX_THREAD_ENTRY(ha_manager_thread, args);

typedef struct
{
	zbx_cuid_t	ha_nodeid;
	zbx_cuid_t	ha_sessionid;
	char		*name;
	char		*address;
	unsigned short	port;
	int		status;
	int		lastaccess;
}
zbx_ha_node_t;

ZBX_PTR_VECTOR_DECL(ha_node, zbx_ha_node_t *)
ZBX_PTR_VECTOR_IMPL(ha_node, zbx_ha_node_t *)

static void	zbx_ha_node_free(zbx_ha_node_t *node)
{
	zbx_free(node->name);
	zbx_free(node->address);
	zbx_free(node);
}

static void	ha_set_error(zbx_ha_info_t *info, const char *fmt, ...) __zbx_attr_format_printf(2, 3);
static DB_RESULT	ha_db_select(zbx_ha_info_t *info, const char *sql, ...) __zbx_attr_format_printf(2, 3);
static int	ha_db_execute(zbx_ha_info_t *info, const char *sql, ...) __zbx_attr_format_printf(2, 3);

/******************************************************************************
 *                                                                            *
 * Function: ha_send_manager_message                                          *
 *                                                                            *
 * Purpose: send message to HA manager                                        *
 *                                                                            *
 ******************************************************************************/
static int	ha_send_manager_message(zbx_uint32_t code, char **error)
{
	if (FAIL == zbx_ipc_async_socket_send(&ha_socket, code, NULL, 0))
	{
		*error = zbx_strdup(NULL, "cannot queue message to HA manager service");
		return FAIL;
	}

	if (FAIL == zbx_ipc_async_socket_flush(&ha_socket, ZBX_HA_SERVICE_TIMEOUT))
	{
		*error = zbx_strdup(NULL, "cannot send message to HA manager service");
		return FAIL;
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_update_parent                                                 *
 *                                                                            *
 * Purpose: update parent process with ha_status and failover delay           *
 *                                                                            *
 ******************************************************************************/
static void	ha_update_parent(zbx_ipc_client_t *client, zbx_ha_info_t *info)
{
	zbx_uint32_t	len = 0, error_len;
	unsigned char	*ptr, *data;
	const char	*error = info->error;
	int		ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() ha_status:%s info:%s", __func__, zbx_ha_status_str(info->ha_status),
			ZBX_NULL2EMPTY_STR(info->error));

	zbx_serialize_prepare_value(len, info->ha_status);
	zbx_serialize_prepare_value(len, info->failover_delay);
	zbx_serialize_prepare_str(len, error);

	ptr = data = (unsigned char *)zbx_malloc(NULL, len);
	ptr += zbx_serialize_value(ptr, info->ha_status);
	ptr += zbx_serialize_value(ptr, info->failover_delay);
	(void)zbx_serialize_str(ptr, error, error_len);

	ret = zbx_ipc_client_send(client, ZBX_IPC_SERVICE_HA_UPDATE, data, len);
	zbx_free(data);

	if (SUCCEED != ret)
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot send HA notification to main process");
		exit(EXIT_FAILURE);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_send_heartbeat                                                *
 *                                                                            *
 * Purpose: send heartbeat message to main process                            *
 *                                                                            *
 ******************************************************************************/
static void	ha_send_heartbeat(zbx_ipc_client_t *client)
{
	if (SUCCEED != zbx_ipc_client_send(client, ZBX_IPC_SERVICE_HA_HEARTBEAT, NULL, 0))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot send HA heartbeat to main process");
		exit(EXIT_FAILURE);
	}
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_set_error                                                 *
 *                                                                            *
 * Purpose: set HA manager error                                              *
 *                                                                            *
 ******************************************************************************/
static void	ha_set_error(zbx_ha_info_t *info, const char *fmt, ...)
{
	va_list	args;
	size_t	len;

	/* don't override errors */
	if (ZBX_NODE_STATUS_ERROR == info->ha_status)
		return;

	va_start(args, fmt);
	len = (size_t)vsnprintf(NULL, 0, fmt, args) + 1;
	va_end(args);

	info->error = (char *)zbx_malloc(info->error, len);

	va_start(args, fmt);
	vsnprintf(info->error, len, fmt, args);
	va_end(args);

	info->ha_status = ZBX_NODE_STATUS_ERROR;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_begin                                                      *
 *                                                                            *
 * Purpose: start database transaction                                        *
 *                                                                            *
 * Comments: Sets error status on non-recoverable database error              *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_begin(zbx_ha_info_t *info)
{
	if (ZBX_DB_DOWN == info->db_status)
		info->db_status = DBconnect(ZBX_DB_CONNECT_ONCE);

	if (ZBX_DB_OK <= info->db_status)
		info->db_status = zbx_db_begin();

	if (ZBX_DB_FAIL == info->db_status)
		ha_set_error(info, "database error");

	return info->db_status;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_rollback                                                   *
 *                                                                            *
 * Purpose: roll back database transaction                                    *
 *                                                                            *
 * Comments: Sets error status on non-recoverable database error              *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_rollback(zbx_ha_info_t *info)
{
	if (ZBX_DB_OK > (info->db_status = zbx_db_rollback()))
	{
		if (ZBX_DB_DOWN == info->db_status)
			DBclose();
	}

	if (ZBX_DB_FAIL == info->db_status)
		ha_set_error(info, "database error");

	return info->db_status;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_commit                                                     *
 *                                                                            *
 * Purpose: commit/rollback database transaction depending on commit result   *
 *                                                                            *
 * Comments: Sets error status on non-recoverable database error              *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_commit(zbx_ha_info_t *info)
{
	if (ZBX_DB_OK <= info->db_status)
		info->db_status = zbx_db_commit();

	if (ZBX_DB_OK > info->db_status)
	{
		zbx_db_rollback();

		if (ZBX_DB_FAIL == info->db_status)
			ha_set_error(info, "database error");
		else
			DBclose();
	}

	return info->db_status;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_select                                                     *
 *                                                                            *
 * Purpose: perform database select sql query based on current database       *
 *          connection status                                                 *
 *                                                                            *
 ******************************************************************************/
static DB_RESULT	ha_db_select(zbx_ha_info_t *info, const char *sql, ...)
{
	va_list		args;
	DB_RESULT	result;

	if (ZBX_DB_OK > info->db_status)
		return NULL;

	va_start(args, sql);
	result = zbx_db_vselect(sql, args);
	va_end(args);

	if (NULL == result)
	{
		info->db_status = ZBX_DB_FAIL;
	}
	else if (ZBX_DB_DOWN == (intptr_t)result)
	{
		info->db_status = ZBX_DB_DOWN;
		result = NULL;
	}

	return result;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_select                                                     *
 *                                                                            *
 * Purpose: perform database sql query based on current database              *
 *          connection status                                                 *
 *                                                                            *
 ******************************************************************************/

static int	ha_db_execute(zbx_ha_info_t *info, const char *sql, ...)
{
	va_list	args;

	if (ZBX_DB_OK > info->db_status)
		return FAIL;

	va_start(args, sql);
	info->db_status = zbx_db_vexecute(sql, args);
	va_end(args);

	return ZBX_DB_OK <= info->db_status ? SUCCEED : FAIL;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_update_config                                              *
 *                                                                            *
 * Purpose: update HA configuration from database                             *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_update_config(zbx_ha_info_t *info)
{
	DB_RESULT	result;
	DB_ROW		row;

	if (NULL == (result = ha_db_select(info, "select ha_failover_delay,auditlog_enabled from config")))
		return FAIL;

	if (NULL != (row = DBfetch(result)))
	{
		if (SUCCEED != is_time_suffix(row[0], &info->failover_delay, ZBX_LENGTH_UNLIMITED))
			THIS_SHOULD_NEVER_HAPPEN;

		info->auditlog = atoi(row[1]);
	}
	else
		THIS_SHOULD_NEVER_HAPPEN;

	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_get_nodes                                                  *
 *                                                                            *
 * Purpose: get all nodes from database                                       *
 *                                                                            *
 * Return value: SUCCEED - the nodes were retrieved from database             *
 *               FAIL    - database/connection error                          *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_get_nodes(zbx_ha_info_t *info, zbx_vector_ha_node_t *nodes, int lock)
{
	DB_RESULT	result;
	DB_ROW		row;

	if (NULL == (result = ha_db_select(info, "select ha_nodeid,name,status,lastaccess,address,port,ha_sessionid"
			" from ha_node order by ha_nodeid%s",
			(0 == lock ? "" : ZBX_FOR_UPDATE))))
	{
		return FAIL;
	}

	while (NULL != (row = DBfetch(result)))
	{
		zbx_ha_node_t	*node;

		node = (zbx_ha_node_t *)zbx_malloc(NULL, sizeof(zbx_ha_node_t));
		zbx_strlcpy(node->ha_nodeid.str, row[0], sizeof(node->ha_nodeid));
		node->name = zbx_strdup(NULL, row[1]);
		node->status = atoi(row[2]);
		node->lastaccess = atoi(row[3]);
		node->address = zbx_strdup(NULL, row[4]);

		if (SUCCEED != is_ushort(row[5], &node->port))
		{
			zabbix_log(LOG_LEVEL_WARNING, "node \"%s\" has invalid port value \"%s\"", row[1], row[5]);
			node->port = 0;
		}

		zbx_strlcpy(node->ha_sessionid.str, row[6], sizeof(node->ha_sessionid));
		zbx_vector_ha_node_append(nodes, node);
	}

	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_check_registered_node                                         *
 *                                                                            *
 * Purpose: check if the node is registered in node table and get ID          *
 *                                                                            *
 ******************************************************************************/
static zbx_ha_node_t	*ha_find_node_by_name(zbx_vector_ha_node_t *nodes, const char *name)
{
	int	i;

	for (i = 0; i < nodes->values_num; i++)
	{
		if (0 == strcmp(nodes->values[i]->name, name))
			return nodes->values[i];
	}

	return NULL;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_get_external_address                                          *
 *                                                                            *
 * Purpose: get server external address and port from configuration           *
 *                                                                            *
 ******************************************************************************/
static void	ha_get_external_address(char **address, unsigned short *port)
{
	(void)parse_serveractive_element(CONFIG_NODE_ADDRESS, address, port, 10051);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_lock_nodes                                                 *
 *                                                                            *
 * Purpose: lock nodes in database                                            *
 *                                                                            *
 * Comments: To lock ha_node table it must have at least one node             *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_lock_nodes(zbx_ha_info_t *info)
{
	DB_RESULT	result;

	if (NULL == (result = ha_db_select(info, "select null from ha_node order by ha_nodeid" ZBX_FOR_UPDATE)))
		return FAIL;

	DBfree_result(result);

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_is_available                                                  *
 *                                                                            *
 * Purpose: check availability based on lastaccess timestamp, database time   *
 *          and failover delay                                                *
 *                                                                            *
 * Return value: SUCCEED - server can be started in active mode               *
 *               FAIL    - server cannot be started based on node registry    *
 *                                                                            *
 ******************************************************************************/
static int	ha_is_available(const zbx_ha_info_t *info, int lastaccess, int db_time)
{
	if (lastaccess + info->failover_delay <= db_time)
		return FAIL;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_check_standalone_config                                       *
 *                                                                            *
 * Purpose: check if server can be started in standalone configuration        *
 *                                                                            *
 * Return value: SUCCEED - server can be started in active mode               *
 *               FAIL    - server cannot be started based on node registry    *
 *                                                                            *
 * Comments: Sets error status on on configuration errors.                    *
 *                                                                            *
 ******************************************************************************/
static int	ha_check_standalone_config(zbx_ha_info_t *info, zbx_vector_ha_node_t *nodes, int db_time)
{
	int	i;

	for (i = 0; i < nodes->values_num; i++)
	{
		if ('\0' == *nodes->values[i]->name)
			continue;

		if (ZBX_NODE_STATUS_STOPPED != nodes->values[i]->status &&
				SUCCEED == ha_is_available(info, nodes->values[i]->lastaccess, db_time))
		{
			ha_set_error(info, "cannot change mode to standalone while HA node \"%s\" is %s",
					nodes->values[i]->name, zbx_ha_status_str(nodes->values[i]->status));
			return FAIL;
		}
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_check_cluster_config                                          *
 *                                                                            *
 * Purpose: check if server can be started in cluster configuration           *
 *                                                                            *
 * Parameters: info     - [IN] - the HA node information                      *
 *             nodes    - [IN] - the cluster nodes                            *
 *             db_time  - [IN] - the current database timestamp               *
 *             activate - [OUT] SUCCEED - start in active mode                *
 *                              FAIL    - start in standby mode               *
 *                                                                            *
 * Return value: SUCCEED - server can be started in returned mode             *
 *               FAIL    - server cannot be started based on node registry    *
 *                                                                            *
 * Comments: Sets error status on on configuration errors.                    *
 *                                                                            *
 ******************************************************************************/
static int	ha_check_cluster_config(zbx_ha_info_t *info, zbx_vector_ha_node_t *nodes, int db_time, int *activate)
{
	int	i;

	*activate = SUCCEED;

	for (i = 0; i < nodes->values_num; i++)
	{
		if (ZBX_NODE_STATUS_STOPPED == nodes->values[i]->status ||
				SUCCEED != ha_is_available(info, nodes->values[i]->lastaccess, db_time))
		{
			continue;
		}

		if ('\0' == *nodes->values[i]->name)
		{
			ha_set_error(info, "cannot change mode to HA while standalone node is %s",
					zbx_ha_status_str(nodes->values[i]->status));
			return FAIL;
		}

		if (0 == strcmp(info->name, nodes->values[i]->name))
		{
			ha_set_error(info, "found %s duplicate \"%s\" node",
					zbx_ha_status_str(nodes->values[i]->status), info->name);
			return FAIL;
		}

		/* immediately switch to active mode if there is no other node that can take over */
		if (ZBX_NODE_STATUS_ACTIVE == nodes->values[i]->status ||
				ZBX_NODE_STATUS_STANDBY == nodes->values[i]->status)
		{
			*activate = FAIL;
		}
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_get_time                                                   *
 *                                                                            *
 * Purpose: get current database time                                         *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_get_time(zbx_ha_info_t *info, int *db_time)
{
	DB_ROW		row;
	DB_RESULT	result;
	int		ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (NULL == (result = ha_db_select(info, "select " ZBX_DB_TIMESTAMP() " from config")))
		goto out;

	if (NULL != (row = DBfetch(result)))
		*db_time = atoi(row[0]);
	else
		*db_time = 0;

	DBfree_result(result);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s db_time:%d", __func__, zbx_result_string(ret),
			SUCCEED == ret ? *db_time : -1);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_flush_audit                                                   *
 *                                                                            *
 * Purpose: flush audit taking in account database connection status          *
 *                                                                            *
 ******************************************************************************/
static void	ha_flush_audit(zbx_ha_info_t *info)
{
	if (ZBX_DB_OK > info->db_status)
	{
		zbx_audit_clean();
		return;
	}

	info->db_status = zbx_audit_flush_once();
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_create_node                                                *
 *                                                                            *
 * Purpose: add new node record in ha_node table if necessary                 *
 *                                                                            *
 * Return value: SUCCEED - node exists, was created or database is offline    *
 *               FAIL    - node configuration or database error               *
 *                                                                            *
 ******************************************************************************/
static void	ha_db_create_node(zbx_ha_info_t *info)
{
	zbx_vector_ha_node_t	nodes;
	int			i, activate, db_time;
	zbx_cuid_t		nodeid;
	char			*name_esc;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_vector_ha_node_create(&nodes);

	if (ZBX_DB_OK > ha_db_begin(info))
		goto finish;

	if (SUCCEED != ha_db_get_nodes(info, &nodes, 0))
		goto out;

	if (FAIL == ha_db_update_config(info))
		goto out;

	for (i = 0; i < nodes.values_num; i++)
	{
		if (0 == strcmp(info->name, nodes.values[i]->name))
		{
			nodeid = nodes.values[i]->ha_nodeid;
			goto out;
		}
	}

	if (SUCCEED != ha_db_get_time(info, &db_time))
		goto out;

	if (ZBX_HA_IS_CLUSTER())
	{
		if (SUCCEED != ha_check_cluster_config(info, &nodes, db_time, &activate))
			goto out;
	}
	else
	{
		if (SUCCEED != ha_check_standalone_config(info, &nodes, db_time))
			goto out;
	}

	zbx_new_cuid(nodeid.str);
	name_esc = DBdyn_escape_string(info->name);

	if (SUCCEED == ha_db_execute(info, "insert into ha_node (ha_nodeid,name,status,lastaccess)"
			" values ('%s','%s',%d," ZBX_DB_TIMESTAMP() ")",
			nodeid.str, name_esc, ZBX_NODE_STATUS_STOPPED))
	{
		zbx_audit_init(info->auditlog);
		zbx_audit_ha_create_entry(AUDIT_ACTION_ADD, nodeid.str, info->name);
		zbx_audit_ha_add_create_fields(nodeid.str, info->name, ZBX_NODE_STATUS_STOPPED);
		ha_flush_audit(info);
	}

	zbx_free(name_esc);
out:
	if (ZBX_NODE_STATUS_ERROR != info->ha_status)
		ha_db_commit(info);
	else
		ha_db_rollback(info);

	if (ZBX_NODE_STATUS_ERROR != info->ha_status)
	{
		if (ZBX_DB_OK <= info->db_status)
			info->ha_nodeid = nodeid;
	}
finish:
	zbx_vector_ha_node_clear_ext(&nodes, zbx_ha_node_free);
	zbx_vector_ha_node_destroy(&nodes);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_register_node                                              *
 *                                                                            *
 * Purpose: register server node                                              *
 *                                                                            *
 * Return value: SUCCEED - node was registered or database was offline        *
 *               FAIL    - fatal error                                        *
 *                                                                            *
 * Comments: If registration was successful the status will be set to either  *
 *           active or standby. If database connection was lost the status    *
 *           will stay unknown until another registration attempt succeeds.   *
 *                                                                            *
 *           In the case of critical error the error status will be set.      *
 *                                                                            *
 ******************************************************************************/
static void	ha_db_register_node(zbx_ha_info_t *info)
{
	zbx_vector_ha_node_t	nodes;
	int			ha_status = ZBX_NODE_STATUS_UNKNOWN, activate = SUCCEED, db_time;
	char			*address = NULL, *sql = NULL;
	size_t			sql_alloc = 0, sql_offset = 0;
	unsigned short		port = 0;
	zbx_ha_node_t		*node;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	zbx_vector_ha_node_create(&nodes);

	ha_db_create_node(info);

	if (SUCCEED == zbx_cuid_empty(info->ha_nodeid))
		goto finish;

	if (ZBX_DB_OK > ha_db_begin(info))
		goto finish;

	if (SUCCEED != ha_db_get_nodes(info, &nodes, ZBX_HA_NODE_LOCK))
		goto out;

	if (SUCCEED != ha_db_get_time(info, &db_time))
		goto out;

	if (ZBX_HA_IS_CLUSTER())
	{
		if (SUCCEED != ha_check_cluster_config(info, &nodes, db_time, &activate))
			goto out;
	}
	else
	{
		if (SUCCEED != ha_check_standalone_config(info, &nodes, db_time))
			goto out;
	}

	if (NULL == (node = ha_find_node_by_name(&nodes, info->name)))
	{
		ha_set_error(info, "cannot find server node \"%s\" in registry", info->name);
		goto out;
	}

	ha_status = SUCCEED == activate ? ZBX_NODE_STATUS_ACTIVE : ZBX_NODE_STATUS_STANDBY;
	ha_get_external_address(&address, &port);

	zbx_audit_init(info->auditlog);
	zbx_audit_ha_create_entry(AUDIT_ACTION_UPDATE, info->ha_nodeid.str, info->name);

	zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "update ha_node set lastaccess="
				ZBX_DB_TIMESTAMP() ",ha_sessionid='%s'", ha_sessionid.str);

	if (ha_status != node->status)
	{
		zbx_audit_ha_update_field_int(info->ha_nodeid.str, ZBX_AUDIT_HA_STATUS, node->status, ha_status);
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ",status=%d", ha_status);
	}

	if (0 != strcmp(address, node->address))
	{
		char	*address_esc;

		address_esc = DBdyn_escape_string(address);
		zbx_audit_ha_update_field_string(node->ha_nodeid.str, ZBX_AUDIT_HA_ADDRESS, node->address, address);
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ",address='%s'", address_esc);
		zbx_free(address_esc);
	}

	if (port != node->port)
	{
		zbx_audit_ha_update_field_int(info->ha_nodeid.str, ZBX_AUDIT_HA_PORT, node->port, port);
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ",port=%d", port);
	}

	ha_db_execute(info, "%s where ha_nodeid='%s'", sql, info->ha_nodeid.str);
	ha_flush_audit(info);

	zbx_free(sql);
	zbx_free(address);
out:
	if (ZBX_NODE_STATUS_ERROR != info->ha_status)
		ha_db_commit(info);
	else
		ha_db_rollback(info);

	if (ZBX_NODE_STATUS_ERROR != info->ha_status)
	{
		if (ZBX_DB_OK <= info->db_status)
			info->ha_status = ha_status;
	}
finish:
	zbx_vector_ha_node_clear_ext(&nodes, zbx_ha_node_free);
	zbx_vector_ha_node_destroy(&nodes);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() nodeid:%s ha_status:%s db_status:%d", __func__,
			info->ha_nodeid.str, zbx_ha_status_str(info->ha_status), info->db_status);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_check_standby_nodes                                           *
 *                                                                            *
 * Purpose: check for standby nodes being unavailable for failrover_delay     *
 *          seconds and mark them unavailable                                 *
 *                                                                            *
 ******************************************************************************/
static int	ha_check_standby_nodes(zbx_ha_info_t *info, zbx_vector_ha_node_t *nodes, int db_time)
{
	int			i, ret = SUCCEED;
	zbx_vector_str_t	unavailable_nodes;

	zbx_audit_init(info->auditlog);

	zbx_vector_str_create(&unavailable_nodes);

	for (i = 0; i < nodes->values_num; i++)
	{
		if (nodes->values[i]->status != ZBX_NODE_STATUS_STANDBY)
			continue;

		if (db_time >= nodes->values[i]->lastaccess + info->failover_delay)
		{
			zbx_vector_str_append(&unavailable_nodes, nodes->values[i]->ha_nodeid.str);

			zbx_audit_ha_create_entry(AUDIT_ACTION_UPDATE, nodes->values[i]->ha_nodeid.str,
					nodes->values[i]->name);
			zbx_audit_ha_update_field_int(nodes->values[i]->ha_nodeid.str, ZBX_AUDIT_HA_STATUS,
					nodes->values[i]->status, ZBX_NODE_STATUS_UNAVAILABLE);
		}
	}

	if (0 != unavailable_nodes.values_num)
	{
		char	*sql = NULL;
		size_t	sql_alloc = 0, sql_offset = 0;

		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "update ha_node set status=%d where",
				ZBX_NODE_STATUS_UNAVAILABLE);

		DBadd_str_condition_alloc(&sql, &sql_alloc, &sql_offset, "ha_nodeid",
				(const char **)unavailable_nodes.values, unavailable_nodes.values_num);

		if (SUCCEED != ha_db_execute(info, "%s", sql))
			ret = FAIL;

		zbx_free(sql);
	}

	zbx_vector_str_destroy(&unavailable_nodes);

	if (SUCCEED == ret)
		ha_flush_audit(info);
	else
		zbx_audit_clean();

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_check_active_node                                             *
 *                                                                            *
 * Purpose: check for active nodes being unavailable for failover_delay       *
 *          seconds, mark them unavailable and set own status to active       *
 *                                                                            *
 ******************************************************************************/
static int	ha_check_active_node(zbx_ha_info_t *info, zbx_vector_ha_node_t *nodes, int *unavailable_index,
		int *ha_status)
{
	int	i, ret = SUCCEED;

	for (i = 0; i < nodes->values_num; i++)
	{
		if (ZBX_NODE_STATUS_ACTIVE == nodes->values[i]->status)
		{
			if ('\0' == *nodes->values[i]->name)
			{
				ha_set_error(info, "found active standalone node in HA mode");
				return FAIL;
			}

			break;
		}
	}

	/* 1) No active nodes - set this node as active.                */
	/* 2) This node is active - update it's status as it might have */
	/*    switched itself to standby mode in the case of prolonged  */
	/*    database connection loss.                                 */
	if (i == nodes->values_num || SUCCEED == zbx_cuid_compare(nodes->values[i]->ha_nodeid, info->ha_nodeid))
	{
		*ha_status = ZBX_NODE_STATUS_ACTIVE;
	}
	else
	{
		if (nodes->values[i]->lastaccess != info->lastaccess_active)
		{
			info->lastaccess_active = nodes->values[i]->lastaccess;
			info->offline_ticks_active = 0;
		}
		else
			info->offline_ticks_active++;

		if (info->failover_delay / ZBX_HA_POLL_PERIOD + 1 < info->offline_ticks_active)
		{
			*unavailable_index = i;
			*ha_status = ZBX_NODE_STATUS_ACTIVE;
		}
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_check_nodes                                                   *
 *                                                                            *
 * Purpose: check HA status based on nodes                                    *
 *                                                                            *
 * Comments: Sets error status on critical errors forcing manager to exit     *
 *                                                                            *
 ******************************************************************************/
static void	ha_check_nodes(zbx_ha_info_t *info)
{
	zbx_vector_ha_node_t	nodes;
	zbx_ha_node_t		*node;
	int			ha_status, db_time, unavailable_index = FAIL;
	char			*sql = NULL;
	size_t			sql_alloc = 0, sql_offset = 0;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() ha_status:%s db_status:%d", __func__, zbx_ha_status_str(info->ha_status),
			info->db_status);

	zbx_vector_ha_node_create(&nodes);

	if (ZBX_DB_OK > ha_db_begin(info))
		goto finish;

	ha_status = info->ha_status;

	if (SUCCEED != ha_db_get_nodes(info, &nodes, ZBX_HA_NODE_LOCK))
		goto out;

	if (NULL == (node = ha_find_node_by_name(&nodes, info->name)))
	{
		ha_set_error(info, "cannot find server node \"%s\" in registry", info->name);
		goto out;
	}

	if (SUCCEED != zbx_cuid_compare(ha_sessionid, node->ha_sessionid))
	{
		ha_set_error(info, "the server HA registry record has changed ownership");
		goto out;
	}

	/* update nodeid after manager restart */
	if (SUCCEED == zbx_cuid_empty(info->ha_nodeid))
		info->ha_nodeid = node->ha_nodeid;

	if (SUCCEED != ha_db_update_config(info))
		goto out;

	if (SUCCEED != ha_db_get_time(info, &db_time))
		goto out;

	if (ZBX_HA_IS_CLUSTER())
	{
		if (ZBX_NODE_STATUS_ACTIVE == info->ha_status)
		{
			if (SUCCEED != ha_check_standby_nodes(info, &nodes, db_time))
				goto out;
		}
		else /* passive status */
		{
			if (SUCCEED != ha_check_active_node(info, &nodes, &unavailable_index, &ha_status))
				goto out;
		}
	}

	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, "update ha_node set lastaccess=" ZBX_DB_TIMESTAMP());

	zbx_audit_init(info->auditlog);

	if (ha_status != node->status)
	{
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, ",status=%d", ha_status);

		zbx_audit_ha_create_entry(AUDIT_ACTION_UPDATE, node->ha_nodeid.str, node->name);
		zbx_audit_ha_update_field_int(node->ha_nodeid.str, ZBX_AUDIT_HA_STATUS, node->status,
				ha_status);
	}

	zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, " where ha_nodeid='%s'", info->ha_nodeid.str);

	if (SUCCEED == ha_db_execute(info, "%s", sql) && FAIL != unavailable_index)
	{
		zbx_ha_node_t	*last_active = nodes.values[unavailable_index];

		ha_db_execute(info, "update ha_node set status=%d where ha_nodeid='%s'",
				ZBX_NODE_STATUS_UNAVAILABLE, last_active->ha_nodeid.str);

		zbx_audit_ha_create_entry(AUDIT_ACTION_UPDATE, last_active->ha_nodeid.str, last_active->name);
		zbx_audit_ha_update_field_int(last_active->ha_nodeid.str, ZBX_AUDIT_HA_STATUS, last_active->status,
				ZBX_NODE_STATUS_UNAVAILABLE);
	}

	ha_flush_audit(info);

	zbx_free(sql);
out:
	if (ZBX_NODE_STATUS_ERROR != info->ha_status)
		ha_db_commit(info);
	else
		ha_db_rollback(info);

	if (ZBX_NODE_STATUS_ERROR != info->ha_status)
	{
		if (ZBX_DB_OK <= info->db_status)
			info->ha_status = ha_status;
	}
finish:
	zbx_vector_ha_node_clear_ext(&nodes, zbx_ha_node_free);
	zbx_vector_ha_node_destroy(&nodes);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() nodeid:%s ha_status:%s db_status:%d", __func__,
			info->ha_nodeid.str, zbx_ha_status_str(info->ha_status), info->db_status);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_update_lastaccess                                          *
 *                                                                            *
 * Purpose: update node lastaccess                                            *
 *                                                                            *
 ******************************************************************************/
static void	ha_db_update_lastaccess(zbx_ha_info_t *info)
{
	zabbix_log(LOG_LEVEL_DEBUG, "In %s() ha_status:%s", __func__, zbx_ha_status_str(info->ha_status));

	if (ZBX_DB_OK > ha_db_begin(info))
		goto out;

	if (SUCCEED == ha_db_lock_nodes(info) &&
			SUCCEED == ha_db_execute(info, "update ha_node set lastaccess=" ZBX_DB_TIMESTAMP()
					" where ha_nodeid='%s'", info->ha_nodeid.str))
	{
		ha_db_commit(info);
	}
	else
		ha_db_rollback(info);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_get_nodes_json                                             *
 *                                                                            *
 * Purpose: get cluster status in lld compatible json format                  *
 *                                                                            *
 ******************************************************************************/
static int	ha_db_get_nodes_json(zbx_ha_info_t *info, char **nodes_json, char **error)
{
	zbx_vector_ha_node_t	nodes;
	int			i, db_time, ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (ZBX_DB_OK > info->db_status)
		goto out;

	if (SUCCEED != ha_db_get_time(info, &db_time))
		goto out;

	zbx_vector_ha_node_create(&nodes);

	if (SUCCEED == ha_db_get_nodes(info, &nodes, 0))
	{
		struct zbx_json	j;
		char		address[512];

		zbx_json_initarray(&j, 1024);

		for (i = 0; i < nodes.values_num; i++)
		{
			zbx_snprintf(address, sizeof(address), "%s:%hu", nodes.values[i]->address,
					nodes.values[i]->port);
			zbx_json_addobject(&j, NULL);

			zbx_json_addstring(&j, ZBX_PROTO_TAG_ID, nodes.values[i]->ha_nodeid.str, ZBX_JSON_TYPE_STRING);
			zbx_json_addstring(&j, ZBX_PROTO_TAG_NAME, nodes.values[i]->name, ZBX_JSON_TYPE_STRING);
			zbx_json_addint64(&j, ZBX_PROTO_TAG_STATUS, (zbx_int64_t)nodes.values[i]->status);
			zbx_json_addint64(&j, ZBX_PROTO_TAG_LASTACCESS, (zbx_int64_t)nodes.values[i]->lastaccess);
			zbx_json_addstring(&j, ZBX_PROTO_TAG_ADDRESS, address, ZBX_JSON_TYPE_STRING);
			zbx_json_addint64(&j, ZBX_PROTO_TAG_DB_TIMESTAMP, (zbx_int64_t)db_time);
			zbx_json_addint64(&j, ZBX_PROTO_TAG_LASTACCESS_AGE,
					(zbx_int64_t)(db_time -nodes.values[i]->lastaccess));

			zbx_json_close(&j);
		}

		*nodes_json = zbx_strdup(NULL, j.buffer);
		zbx_json_free(&j);

		ret = SUCCEED;
	}

	zbx_vector_ha_node_clear_ext(&nodes, zbx_ha_node_free);
	zbx_vector_ha_node_destroy(&nodes);
out:
	if (SUCCEED != ret)
		*error = zbx_strdup(NULL, "database error");

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_remove_node_by_index                                          *
 *                                                                            *
 * Purpose: remove node by its index in node list                             *
 *                                                                            *
 ******************************************************************************/
static int	ha_remove_node_by_index(zbx_ha_info_t *info, int index, char **error)
{
	zbx_vector_ha_node_t	nodes;
	int			ret = FAIL;

	if (ZBX_DB_OK > ha_db_begin(info))
	{
		*error = zbx_strdup(NULL, "database connection problem");
		return FAIL;
	}

	zbx_vector_ha_node_create(&nodes);

	if (SUCCEED != ha_db_get_nodes(info, &nodes, 0))
	{
		*error = zbx_strdup(NULL, "database connection problem");
		goto out;
	}

	index--;

	if (0 > index || index >= nodes.values_num)
	{
		*error = zbx_strdup(NULL, "node index out of range");
		goto out;
	}

	if (ZBX_NODE_STATUS_ACTIVE == nodes.values[index]->status ||
			ZBX_NODE_STATUS_STANDBY == nodes.values[index]->status)
	{
		*error = zbx_dsprintf(NULL, "node is %s", zbx_ha_status_str(nodes.values[index]->status));
		goto out;
	}

	if (SUCCEED != ha_db_execute(info, "delete from ha_node where ha_nodeid='%s'", nodes.values[index]->ha_nodeid.str))
	{
		*error = zbx_strdup(NULL, "database connection problem");
		goto out;
	}
	else
	{
		zbx_audit_init(info->auditlog);
		zbx_audit_ha_create_entry(AUDIT_ACTION_DELETE, nodes.values[index]->ha_nodeid.str,
				nodes.values[index]->name);
		ha_flush_audit(info);
	}

	ret = SUCCEED;
out:
	if (SUCCEED == ret)
	{
		if (ZBX_DB_OK <= ha_db_commit(info))
		{
			zabbix_log(LOG_LEVEL_WARNING, "removed node \"%s\" with ID \"%s\"", nodes.values[index]->name,
					nodes.values[index]->ha_nodeid.str);
		}
	}
	else
		ha_db_rollback(info);

	zbx_vector_ha_node_clear_ext(&nodes, zbx_ha_node_free);
	zbx_vector_ha_node_destroy(&nodes);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: ha_report_cluster_status                                         *
 *                                                                            *
 * Purpose: report cluster status in log file                                 *
 *                                                                            *
 ******************************************************************************/
static void	ha_remove_node(zbx_ha_info_t *info, zbx_ipc_client_t *client, const zbx_ipc_message_t *message)
{
	int		index;
	char		*error = NULL;
	zbx_uint32_t	len = 0, error_len;
	unsigned char	*data;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	memcpy(&index, message->data, sizeof(index));

	ha_remove_node_by_index(info, index, &error);

	zbx_serialize_prepare_str(len, error);

	data = zbx_malloc(NULL, len);
	zbx_serialize_str(data, error, error_len);
	zbx_free(error);

	zbx_ipc_client_send(client, ZBX_IPC_SERVICE_HA_REMOVE_NODE, data, len);
	zbx_free(data);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_set_failover_delay                                            *
 *                                                                            *
 * Purpose: set failover delay                                                *
 *                                                                            *
 ******************************************************************************/
static void	ha_set_failover_delay(zbx_ha_info_t *info, zbx_ipc_client_t *client, const zbx_ipc_message_t *message)
{
	int		delay;
	const char	*error = NULL;
	zbx_uint32_t	len = 0, error_len;
	unsigned char	*data;
	DB_RESULT	result;
	DB_ROW		row;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (NULL == (result = ha_db_select(info, "select configid,ha_failover_delay from config")))
	{
		error = "database error";
		goto out;
	}

	memcpy(&delay, message->data, sizeof(delay));

	if (NULL != (row = DBfetch(result)) &&
		SUCCEED == ha_db_execute(info, "update config set ha_failover_delay=%d", delay))
	{
		zbx_uint64_t	configid;

		info->failover_delay = delay;
		zabbix_log(LOG_LEVEL_WARNING, "HA failover delay set to %ds", delay);

		ZBX_STR2UINT64(configid, row[0]);
		zbx_audit_init(info->auditlog);
		zbx_audit_settings_create_entry(AUDIT_ACTION_UPDATE, configid);
		zbx_audit_update_json_update_int(configid, AUDIT_CONFIG_ID, "settings.ha_failover_delay", atoi(row[1]),
				delay);
		ha_flush_audit(info);
	}
	else
		error = "database error";

	DBfree_result(result);
out:
	zbx_serialize_prepare_str(len, error);

	data = zbx_malloc(NULL, len);
	zbx_serialize_str(data, error, error_len);

	zbx_ipc_client_send(client, ZBX_IPC_SERVICE_HA_SET_FAILOVER_DELAY, data, len);
	zbx_free(data);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_send_node_list                                               *
 *                                                                            *
 * Purpose: reply to get nodes request                                        *
 *                                                                            *
 ******************************************************************************/
static void	ha_send_node_list(zbx_ha_info_t *info, zbx_ipc_client_t *client)
{
	int		ret;
	char		*error = NULL, *nodes_json = NULL, *str;
	zbx_uint32_t	len = 0, str_len;
	unsigned char	*data, *ptr;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (SUCCEED == (ret = ha_db_get_nodes_json(info, &nodes_json, &error)))
		str = nodes_json;
	else
		str = error;

	zbx_serialize_prepare_value(len, ret);
	zbx_serialize_prepare_str(len, str);

	ptr = data = zbx_malloc(NULL, len);
	ptr += zbx_serialize_value(ptr, ret);
	(void)zbx_serialize_str(ptr, str, str_len);
	zbx_free(str);

	zbx_ipc_client_send(client, ZBX_IPC_SERVICE_HA_GET_NODES, data, len);
	zbx_free(data);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __func__);
}

/******************************************************************************
 *                                                                            *
 * Function: ha_db_update_exit_status                                         *
 *                                                                            *
 * Purpose: update node status in database on shutdown                        *
 *                                                                            *
 ******************************************************************************/
static void	ha_db_update_exit_status(zbx_ha_info_t *info)
{
	if (ZBX_NODE_STATUS_ACTIVE != info->ha_status && ZBX_NODE_STATUS_STANDBY != info->ha_status)
		return;

	if (ZBX_DB_OK > ha_db_begin(info))
		return;

	if (SUCCEED != ha_db_lock_nodes(info))
		goto out;

	if (SUCCEED == ha_db_execute(info, "update ha_node set status=%d where ha_nodeid='%s'",
			ZBX_NODE_STATUS_STOPPED, info->ha_nodeid.str))
	{
		zbx_audit_init(info->auditlog);
		zbx_audit_ha_create_entry(AUDIT_ACTION_UPDATE, info->ha_nodeid.str, info->name);
		zbx_audit_ha_update_field_int(info->ha_nodeid.str, ZBX_AUDIT_HA_STATUS, info->ha_status, ZBX_NODE_STATUS_STOPPED);
		ha_flush_audit(info);
	}
out:
	ha_db_commit(info);
}

/*
 * public API
 */

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_get_status                                                *
 *                                                                            *
 * Purpose: requests HA manager to send status update                         *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_get_status(char **error)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = ha_send_manager_message(ZBX_IPC_SERVICE_HA_UPDATE, error);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_recv_status                                               *
 *                                                                            *
 * Purpose: handle HA manager notifications                                   *
 *                                                                            *
 * Comments: This function also monitors heartbeat notifications and          *
 *           returns standby status if no heartbeats are received for         *
 *           failover delay - poll period seconds. This would make main       *
 *           process to switch to standby mode and initiate teardown process  *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_recv_status(int timeout, int *ha_status, char **error)
{
	zbx_ipc_message_t	*message = NULL;
	int			ret = SUCCEED, ha_status_old;
	time_t			now;
	static time_t		last_hb;
	static int		ha_failover_delay = ZBX_HA_DEFAULT_FAILOVER_DELAY;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	while (1)
	{
		unsigned char	*ptr;
		zbx_uint32_t	len;

		if (SUCCEED != zbx_ipc_async_socket_recv(&ha_socket, timeout, &message))
		{
			*ha_status = ZBX_NODE_STATUS_ERROR;
			*error = zbx_strdup(NULL, "cannot receive message from HA manager service");
			ret = FAIL;
			goto out;
		}

		now = time(NULL);

		if (NULL == message)
			break;

		switch (message->code)
		{
			case ZBX_IPC_SERVICE_HA_UPDATE:
				ha_status_old = *ha_status;

				ptr = message->data;
				ptr += zbx_deserialize_value(ptr, ha_status);
				ptr += zbx_deserialize_value(ptr, &ha_failover_delay);
				(void)zbx_deserialize_str(ptr, error, len);

				if (ZBX_NODE_STATUS_ERROR == *ha_status)
				{
					zbx_ipc_message_free(message);
					ret = FAIL;
					goto out;
				}

				/* reset heartbeat on status change */
				if (ha_status_old != *ha_status)
					last_hb = now;

				break;
			case ZBX_IPC_SERVICE_HA_HEARTBEAT:
				last_hb = now;
				break;
		}

		zbx_ipc_message_free(message);

		/* reset timeout for getting pending messages */
		timeout = 0;
	}

	if (ZBX_HA_IS_CLUSTER() && *ha_status == ZBX_NODE_STATUS_ACTIVE && 0 != last_hb)
	{
		if (last_hb + ha_failover_delay - ZBX_HA_POLL_PERIOD <= now || now < last_hb)
			*ha_status = ZBX_NODE_STATUS_STANDBY;
	}
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_remove_node                                               *
 *                                                                            *
 * Purpose: remove HA node                                                    *
 *                                                                            *
 * Comments: A new socket is opened to avoid interfering with notification    *
 *           channel                                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_remove_node(int node_num, char **error)
{
	unsigned char		*data;
	zbx_uint32_t		error_len;

	if (SUCCEED != zbx_ipc_async_exchange(ZBX_IPC_SERVICE_HA, ZBX_IPC_SERVICE_HA_REMOVE_NODE,
			ZBX_HA_SERVICE_TIMEOUT, (unsigned char *)&node_num, sizeof(node_num), &data, error))
	{
		return FAIL;
	}

	(void)zbx_deserialize_str(data, error, error_len);
	zbx_free(data);

	return (0 == error_len ? SUCCEED : FAIL);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_set_failover_delay                                        *
 *                                                                            *
 * Purpose: set HA failover delay                                             *
 *                                                                            *
 * Comments: A new socket is opened to avoid interfering with notification    *
 *           channel                                                          *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_set_failover_delay(int delay, char **error)
{
	unsigned char		*data;
	zbx_uint32_t		error_len;

	if (SUCCEED != zbx_ipc_async_exchange(ZBX_IPC_SERVICE_HA, ZBX_IPC_SERVICE_HA_SET_FAILOVER_DELAY,
			ZBX_HA_SERVICE_TIMEOUT, (unsigned char *)&delay, sizeof(delay), &data, error))
	{
		return FAIL;
	}

	(void)zbx_deserialize_str(data, error, error_len);
	zbx_free(data);

	return (0 == error_len ? SUCCEED : FAIL);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_start                                                     *
 *                                                                            *
 * Purpose: start HA manager                                                  *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_start(char **error, int ha_status)
{
	char			*errmsg = NULL;
	int			ret = FAIL;
	zbx_thread_args_t	args;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	args.args = (void *)(uintptr_t)ha_status;
	zbx_thread_start(ha_manager_thread, &args, &ha_pid);

	if (ZBX_THREAD_ERROR == ha_pid)
	{
		*error = zbx_dsprintf(NULL, "cannot create HA manager process: %s", zbx_strerror(errno));
		goto out;
	}

	if (SUCCEED != zbx_ipc_async_socket_open(&ha_socket, ZBX_IPC_SERVICE_HA, ZBX_HA_SERVICE_TIMEOUT, &errmsg))
	{
		*error = zbx_dsprintf(NULL, "cannot connect to HA manager process: %s", errmsg);
		zbx_free(errmsg);
		goto out;
	}

	if (FAIL == zbx_ipc_async_socket_send(&ha_socket, ZBX_IPC_SERVICE_HA_REGISTER, NULL, 0))
	{
		*error = zbx_dsprintf(NULL, "cannot queue message to HA manager service");
		goto out;
	}

	if (FAIL == zbx_ipc_async_socket_flush(&ha_socket, ZBX_HA_SERVICE_TIMEOUT))
	{
		*error = zbx_dsprintf(NULL, "cannot send message to HA manager service");
		goto out;
	}

	ret = SUCCEED;
out:
	if (SUCCEED != ret && ZBX_THREAD_ERROR != ha_pid)
		zbx_ha_kill();

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_pause                                                     *
 *                                                                            *
 * Purpose: pause HA manager                                                  *
 *                                                                            *
 * Comments: HA manager must be paused before stopping it normally            *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_pause(char **error)
{
	int	ret;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	ret = ha_send_manager_message(ZBX_IPC_SERVICE_HA_PAUSE, error);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_stop                                                      *
 *                                                                            *
 * Purpose: stop  HA manager                                                  *
 *                                                                            *
 * Comments: This function is used to stop HA manager on normal shutdown      *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_stop(char **error)
{
	int	ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (ZBX_THREAD_ERROR == ha_pid)
	{
		ret = SUCCEED;
		goto out;
	}

	if (SUCCEED == ha_send_manager_message(ZBX_IPC_SERVICE_HA_STOP, error))
	{
		if (ZBX_THREAD_ERROR == zbx_thread_wait(ha_pid))
		{
			*error = zbx_dsprintf(NULL, "failed to wait for HA manager to exit: %s", zbx_strerror(errno));
			goto out;
		}

		ret = SUCCEED;
	}
out:
	ha_pid = ZBX_THREAD_ERROR;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_change_loglevel                                           *
 *                                                                            *
 * Purpose: change HA manager log level                                       *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_change_loglevel(int direction, char **error)
{
	int		ret = FAIL;
	zbx_uint32_t	cmd;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __func__);

	if (ZBX_THREAD_ERROR == ha_pid)
	{
		*error = zbx_strdup(NULL, "HA manager has not been started");
		goto out;
	}

	cmd = 0 < direction ? ZBX_IPC_SERVICE_HA_LOGLEVEL_INCREASE :  ZBX_IPC_SERVICE_HA_LOGLEVEL_DECREASE;

	ret = ha_send_manager_message(cmd, error);
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __func__, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_kill                                                      *
 *                                                                            *
 * Purpose: kill HA manager                                                   *
 *                                                                            *
 ******************************************************************************/
void	zbx_ha_kill(void)
{
	kill(ha_pid, SIGKILL);
	zbx_thread_wait(ha_pid);
	ha_pid = ZBX_THREAD_ERROR;

	if (SUCCEED == zbx_ipc_async_socket_connected(&ha_socket))
		zbx_ipc_async_socket_close(&ha_socket);
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_status_str                                                *
 *                                                                            *
 * Purpose: get HA status in text format                                      *
 *                                                                            *
 ******************************************************************************/
const char	*zbx_ha_status_str(int ha_status)
{
	switch (ha_status)
	{
		case ZBX_NODE_STATUS_STANDBY:
			return "standby";
		case ZBX_NODE_STATUS_STOPPED:
			return "stopped";
		case ZBX_NODE_STATUS_UNAVAILABLE:
			return "unavailable";
		case ZBX_NODE_STATUS_ACTIVE:
			return "active";
		case ZBX_NODE_STATUS_ERROR:
			return "error";
		default:
			return "unknown";
	}
}

/******************************************************************************
 *                                                                            *
 * Function: zbx_ha_check_pid                                                 *
 *                                                                            *
 * Purpose: check if the pid is HA manager pid                                *
 *                                                                            *
 ******************************************************************************/
int	zbx_ha_check_pid(pid_t pid)
{
	return pid == ha_pid ? SUCCEED : FAIL;
}

/*
 * main process loop
 */
ZBX_THREAD_ENTRY(ha_manager_thread, args)
{
	zbx_ipc_service_t	service;
	char			*error = NULL;
	zbx_ipc_client_t	*client, *main_proc = NULL;
	zbx_ipc_message_t	*message;
	int			pause = FAIL, stop = FAIL, ticks_num = 0, nextcheck;
	double			now, tick;
	zbx_ha_info_t		info;
	zbx_timespec_t		timeout;

	zbx_setproctitle("ha manager");

	zabbix_log(LOG_LEVEL_INFORMATION, "starting HA manager");

	if (FAIL == zbx_ipc_service_start(&service, ZBX_IPC_SERVICE_HA, &error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot start HA manager service: %s", error);
		zbx_free(error);
		exit(EXIT_FAILURE);
	}

	zbx_cuid_clear(info.ha_nodeid);
	info.name = ZBX_NULL2EMPTY_STR(CONFIG_HA_NODE_NAME);
	info.ha_status = (int)(uintptr_t)((zbx_thread_args_t *)args)->args;
	info.error = NULL;
	info.db_status = ZBX_DB_DOWN;
	info.offline_ticks_active = 0;
	info.lastaccess_active = 0;
	info.failover_delay = ZBX_HA_DEFAULT_FAILOVER_DELAY;
	info.auditlog = 0;

	tick = zbx_time();

	if (ZBX_NODE_STATUS_UNKNOWN == info.ha_status)
	{
		ha_db_register_node(&info);

		if (ZBX_NODE_STATUS_ERROR == info.ha_status)
			goto pause;
	}

	nextcheck = ZBX_HA_POLL_PERIOD;

	/* double the initial database check delay in standby mode to avoid the same node becoming active */
	/* immediately after switching to standby mode or crashing and being restarted                    */
	if (ZBX_NODE_STATUS_STANDBY == info.ha_status)
		nextcheck *= 2;

	zabbix_log(LOG_LEVEL_INFORMATION, "HA manager started in %s mode", zbx_ha_status_str(info.ha_status));

	while (SUCCEED != pause && ZBX_NODE_STATUS_ERROR != info.ha_status)
	{
		if (tick <= (now = zbx_time()))
		{
			ticks_num++;

			if (nextcheck <= ticks_num)
			{
				int	old_status = info.ha_status, delay;

				if (ZBX_NODE_STATUS_UNKNOWN == info.ha_status)
					ha_db_register_node(&info);
				else
					ha_check_nodes(&info);

				if (NULL != main_proc)
				{
					if (old_status != info.ha_status && ZBX_NODE_STATUS_UNKNOWN != info.ha_status)
						ha_update_parent(main_proc, &info);
				}

				if (ZBX_NODE_STATUS_ERROR == info.ha_status)
					break;

				/* in offline mode try connecting to database every second otherwise */
				/* with small failover delay (10s) it might switch to standby mode   */
				/* despite connection being restored shortly                         */
				delay = ZBX_DB_OK <= info.db_status ? ZBX_HA_POLL_PERIOD : 1;

				while (nextcheck <= ticks_num)
					nextcheck += delay;
			}

			if (NULL != main_proc && ZBX_DB_OK <= info.db_status)
				ha_send_heartbeat(main_proc);

			while (tick <= now)
				tick++;
		}

		timeout.sec = (int)(tick - now);
		timeout.ns = (int)((tick - now) * 1000000000) % 1000000000;

		(void)zbx_ipc_service_recv(&service, &timeout, &client, &message);

		if (NULL != message)
		{
			switch (message->code)
			{
				case ZBX_IPC_SERVICE_HA_REGISTER:
					main_proc = client;
					break;
				case ZBX_IPC_SERVICE_HA_UPDATE:
					ha_update_parent(main_proc, &info);
					break;
				case ZBX_IPC_SERVICE_HA_STOP:
					stop = SUCCEED;
					ZBX_FALLTHROUGH;
				case ZBX_IPC_SERVICE_HA_PAUSE:
					pause = SUCCEED;
					break;
				case ZBX_IPC_SERVICE_HA_GET_NODES:
					ha_send_node_list(&info, client);
					break;
				case ZBX_IPC_SERVICE_HA_REMOVE_NODE:
					ha_remove_node(&info, client, message);
					break;
				case ZBX_IPC_SERVICE_HA_SET_FAILOVER_DELAY:
					ha_set_failover_delay(&info, client, message);
					ha_update_parent(main_proc, &info);
					break;
				case ZBX_IPC_SERVICE_HA_LOGLEVEL_INCREASE:
					if (SUCCEED != zabbix_increase_log_level())
					{
						zabbix_log(LOG_LEVEL_INFORMATION, "cannot increase log level:"
								" maximum level has been already set");
					}
					else
					{
						zabbix_log(LOG_LEVEL_INFORMATION, "log level has been increased to %s",
								zabbix_get_log_level_string());
					}
					break;
				case ZBX_IPC_SERVICE_HA_LOGLEVEL_DECREASE:
					if (SUCCEED != zabbix_decrease_log_level())
					{
						zabbix_log(LOG_LEVEL_INFORMATION, "cannot decrease log level:"
								" minimum level has been already set");
					}
					else
					{
						zabbix_log(LOG_LEVEL_INFORMATION, "log level has been decreased to %s",
								zabbix_get_log_level_string());
					}
					break;
			}

			zbx_ipc_message_free(message);
		}

		if (NULL != client)
			zbx_ipc_client_release(client);
	}

	zabbix_log(LOG_LEVEL_INFORMATION, "HA manager has been paused");
pause:
	timeout.sec = ZBX_HA_POLL_PERIOD;
	timeout.ns = 0;

	while (SUCCEED != stop)
	{
		(void)zbx_ipc_service_recv(&service, &timeout, &client, &message);

		if (ZBX_NODE_STATUS_STANDBY == info.ha_status || ZBX_NODE_STATUS_ACTIVE == info.ha_status)
			ha_db_update_lastaccess(&info);

		if (NULL != message)
		{
			switch (message->code)
			{
				case ZBX_IPC_SERVICE_HA_REGISTER:
					main_proc = client;
					break;
				case ZBX_IPC_SERVICE_HA_UPDATE:
					ha_update_parent(main_proc, &info);
					break;
				case ZBX_IPC_SERVICE_HA_STOP:
					stop = SUCCEED;
					break;
			}

			zbx_ipc_message_free(message);
		}

		if (NULL != client)
			zbx_ipc_client_release(client);
	}

	zbx_free(info.error);

	ha_db_update_exit_status(&info);

	DBclose();

	zbx_ipc_service_close(&service);

	zabbix_log(LOG_LEVEL_INFORMATION, "HA manager has been stopped");

	exit(EXIT_SUCCESS);

	return 0;
}
