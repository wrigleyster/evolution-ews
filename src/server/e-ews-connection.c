/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Authors :
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <string.h>
#include <ctype.h>
#include <glib/gi18n-lib.h>
#include "e-ews-connection.h"
#include "e-ews-message.h"
#include "e-ews-folder.h"

#define d(x) x

/* For the number of connections */
#define EWS_CONNECTIONS_NUMBER 2

#define QUEUE_LOCK(x) (g_static_rec_mutex_lock(&(x)->priv->queue_lock))
#define QUEUE_UNLOCK(x) (g_static_rec_mutex_unlock(&(x)->priv->queue_lock))

G_DEFINE_TYPE (EEwsConnection, e_ews_connection, G_TYPE_OBJECT)

static GObjectClass *parent_class = NULL;
static GHashTable *loaded_connections_permissions = NULL;
static void ews_next_request (EEwsConnection *cnc);
static gint comp_func (gconstpointer a, gconstpointer b);
static GQuark ews_connection_error_quark (void);
typedef void (*response_cb) (SoupSession *session, SoupMessage *msg, gpointer user_data);
static void 
ews_connection_authenticate	(SoupSession *sess, SoupMessage *msg,
				 SoupAuth *auth, gboolean retrying, 
				 gpointer data);

/* Connection APIS */

#define  EWS_CONNECTION_ERROR \
         (ews_connection_error_quark ())

struct _EEwsConnectionPrivate {
	SoupSession *soup_session;

	gchar *uri;
	gchar *username;
	gchar *password;

	GSList *jobs;
	GSList *active_job_queue;
	GStaticRecMutex queue_lock;
};

enum {
	NEXT_REQUEST,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef struct _EWSNode EWSNode;
typedef struct _EwsAsyncData EwsAsyncData;

struct _EwsAsyncData {
	GList *folders;	
};

struct _EWSNode {
	ESoapMessage *msg;
	EEwsConnection *cnc;
	GSimpleAsyncResult *simple;

	gint pri;		/* the command priority */
	response_cb cb;
};

typedef struct {
  GAsyncResult *res;
  GMainContext *context;
  GMainLoop *loop;
} EwsSyncData;

/* Static Functions */

static GQuark
ews_connection_error_quark (void)
{
	static GQuark quark = 0;

	if (G_UNLIKELY (quark == 0)) {
		const gchar *string = "ews-connection-error-quark";
		quark = g_quark_from_static_string (string);
	}

	return quark;
}

static void
async_data_free (EwsAsyncData *async_data)
{
	g_free (async_data);
}

static void
ews_sync_reply_cb	(GObject *object,
			 GAsyncResult *res,
			 gpointer user_data)
{

  EwsSyncData *sync_data = user_data;

  sync_data->res = g_object_ref (res);
  g_main_loop_quit (sync_data->loop);
}

static EWSNode *
ews_node_new ()
{
	EWSNode *node;

	node = g_malloc0 (sizeof (EWSNode));
	return node;
}

static gchar*
autodiscover_parse_protocol(xmlNode *node)
{
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "ASUrl")) {
			char *asurl = (char *)xmlNodeGetContent(node);
			if (asurl) {
				printf("Got ASUrl %s\n", asurl);
				return asurl;
			}
		}
	}
	return NULL;
}

static gint
comp_func (gconstpointer a, gconstpointer b)
{
	EWSNode *node1 = (EWSNode *) a;
	EWSNode *node2 = (EWSNode *) b;

	g_print ("\tSorting based on priority...\n");
	if (node1->pri > node2->pri)
		return 1;
	else
		return -1;
}

static void
e_ews_connection_queue_message (EEwsConnection *cnc, ESoapMessage *msg, SoupSessionCallback callback,
			   gpointer user_data)
{
	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));
	g_return_if_fail (E_IS_SOAP_MESSAGE (msg));

	QUEUE_LOCK (cnc);

	soup_session_queue_message (cnc->priv->soup_session, SOUP_MESSAGE (msg), callback, user_data);

	QUEUE_UNLOCK (cnc);
}

static void
ews_next_request (EEwsConnection *cnc)
{
	GSList *l;
	EWSNode *node;

	QUEUE_LOCK (cnc);

	l = cnc->priv->jobs;
	node = (EWSNode *) l->data;

	if (g_slist_length (cnc->priv->active_job_queue) >= EWS_CONNECTIONS_NUMBER) {
		QUEUE_UNLOCK (cnc);
		return;
	}

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) == 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (node->msg)->request_body));
		/* print request's body */
		printf ("\n The request headers");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (node->msg)->request_body->data, stdout);
		fputc ('\n', stdout);
	}

	/* Remove the node from the priority queue */
	cnc->priv->jobs = g_slist_remove (cnc->priv->jobs, (gconstpointer *) node);

	/* Add to active job queue */
	cnc->priv->active_job_queue = g_slist_append (cnc->priv->active_job_queue, node);
	e_ews_connection_queue_message (cnc, node->msg, node->cb, node);

	QUEUE_UNLOCK (cnc);
}

/**
 * ews_active_job_done 
 * @cnc: 
 * @msg: 
 * Removes the node from active Queue and free's the node	 
 * 
 * Returns: 
 **/
static gboolean
ews_active_job_done (EEwsConnection *cnc, SoupMessage *msg)
{
	EWSNode *ews_node = NULL;
	GSList *l = NULL;
	gboolean found = FALSE;

	QUEUE_LOCK (cnc);

	for (l = cnc->priv->active_job_queue; l!= NULL ;l = g_slist_next (l)) {
		ews_node = (EWSNode *) l->data;
		if (SOUP_MESSAGE (ews_node->msg) == msg) {
			found = TRUE;
			cnc->priv->active_job_queue = g_slist_remove (cnc->priv->active_job_queue, ews_node);
			break;
		}
	}
	
	QUEUE_UNLOCK (cnc);

	g_free (ews_node);
	return found;
}

static void 
ews_cancel_request (GCancellable *cancellable,
		   gpointer user_data)
{
	EWSNode *node = user_data;
	EEwsConnection *cnc = node->cnc;
	GSimpleAsyncResult *simple = node->simple;
	ESoapMessage *msg = node->msg;
	gboolean found = FALSE;

	g_print ("\nCanceled this request\n");

	found = ews_active_job_done (cnc, SOUP_MESSAGE (node->msg));
	if (found)
		soup_session_cancel_message (cnc->priv->soup_session, SOUP_MESSAGE (msg), SOUP_STATUS_CANCELLED);
	else {
		QUEUE_LOCK (cnc);
		cnc->priv->jobs = g_slist_remove (cnc->priv->jobs, (gconstpointer) node);
		QUEUE_UNLOCK (cnc);

		g_free (node);
	}

	g_simple_async_result_set_error	(simple,
					 EWS_CONNECTION_ERROR,
					 EWS_CONNECTION_STATUS_CANCELLED,
					 _("Operation Cancelled"));
	g_simple_async_result_complete_in_idle (simple);
}

static void
ews_connection_queue_request (EEwsConnection *cnc, ESoapMessage *msg, response_cb cb, gint pri, GCancellable *cancellable, GSimpleAsyncResult *simple)
{
	EWSNode *node;

	node = ews_node_new ();
	node->msg = msg;
	node->pri = pri;
	node->cb = cb;
	node->cnc = cnc;
	node->simple = simple;

	QUEUE_LOCK (cnc);
	cnc->priv->jobs = g_slist_insert_sorted (cnc->priv->jobs, (gpointer *) node, (GCompareFunc) comp_func);
 	QUEUE_UNLOCK (cnc);

	if (cancellable)
		g_cancellable_connect	(cancellable,
					 G_CALLBACK (ews_cancel_request),
					 (gpointer) node, NULL);

	g_signal_emit	(cnc, signals[NEXT_REQUEST], 0);
}

/* Response callbacks */

/* Just dump the response for debugging */
static void
dump_response_cb (SoupSession *session, SoupMessage *msg, gpointer data)
{
	ESoapResponse *response;
	EEwsConnection *cnc = (EEwsConnection *) data;
	gboolean found = FALSE;

	soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (msg)->request_body));
	/* print request's body */
	g_print ("\n------\n");
	fputs (SOUP_MESSAGE (msg)->request_body->data, stdout);
	fputc ('\n', stdout);

	response = e_soap_message_parse_response ((ESoapMessage *) msg);

	if (response) {
		/* README: The stdout can be replaced with Evolution's
		Logging framework also */

		e_soap_response_dump_response (response, stdout);
		g_print ("\n------\n");
	} else
		return;

	found = ews_active_job_done (cnc, msg);
	/* free memory */
	g_object_unref (response);
}

static guint
get_response_status (ESoapParameter *param)
{
	ESoapParameter *subparam;
	gchar *value;

	value = e_soap_parameter_get_property (param, "ResponseClass");

	if (!strcmp (value, "Error")) {
		g_free (value);

		g_print ("\nNegative case\n");

		subparam = e_soap_parameter_get_first_child_by_name (param, "MessageText");
		value = e_soap_parameter_get_string_value (subparam);
		g_print ("\nThe message text:\n\t%s", value);
		g_free (value);

		subparam = e_soap_parameter_get_first_child_by_name (param, "ResponseCode");
		value = e_soap_parameter_get_string_value (subparam);
		g_print ("\nThe response code:\n\t%s", value);
		g_free (value);
		
		return 0;
	}

	g_free (value);

	return 1;
}

static void
create_folder_response_cb (SoupSession *session, SoupMessage *msg, gpointer data)
{
	ESoapResponse *response;
	EEwsConnection *cnc = (EEwsConnection *) data;
	ESoapParameter *param, *subparam, *node;
	gboolean test, found = FALSE;
	gchar *value;

	response = e_soap_message_parse_response ((ESoapMessage *) msg);
	if (!response)
		return;

	if (response && g_getenv ("EWS_DEBUG")) {
		/* README: The stdout can be replaced with Evolution's
		Logging framework also */

		e_soap_response_dump_response (response, stdout);
		g_print ("\n------\n");
	}

	param = e_soap_response_get_first_parameter_by_name (response, "ResponseMessages");
	subparam = e_soap_parameter_get_first_child_by_name (param, "CreateFolderResponseMessage");
	test = get_response_status (subparam);
	if (!test)
		goto error;

	node = e_soap_parameter_get_first_child_by_name (subparam, "ResponseCode");

	/* Negative cases */
	if (strcmp (e_soap_parameter_get_string_value(node), "NoError") != EWS_CONNECTION_STATUS_OK) {
		/* free memory */
		g_object_unref (response);
	}

	node = e_soap_parameter_get_first_child_by_name (subparam, "Folders");
	node = e_soap_parameter_get_first_child_by_name (node, "Folder");
	subparam = e_soap_parameter_get_first_child_by_name (node, "FolderId");

	value = e_soap_parameter_get_property (subparam, "Id");
	g_print ("\nThe folder id is...%s\n", value);
	g_free (value);
	
error:
	/* free memory */
	g_object_unref (response);

	found = ews_active_job_done (cnc, msg);
}

static void
sync_hierarchy_response_cb (SoupSession *session, SoupMessage *msg, gpointer data)
{
	ESoapResponse *response;
	EWSNode *enode = (EWSNode *) data;	
	EEwsConnection *cnc = enode->cnc;
	ESoapParameter *param, *subparam, *node;
	EwsAsyncData *async_data;
	const gchar *new_sync_state = NULL;
	gboolean found = FALSE;
	GList *folders = NULL;

	response = e_soap_message_parse_response ((ESoapMessage *) msg);

	if (!response)
		return;

	if (response && g_getenv ("EWS_DEBUG")) {
		/* README: The stdout can be replaced with Evolution's
		Logging framework also */

		e_soap_response_dump_response (response, stdout);
		g_print ("\n------\n");
	}

	param = e_soap_response_get_first_parameter_by_name (response, "ResponseMessages");
	subparam = e_soap_parameter_get_first_child_by_name (param, "SyncFolderHierarchyResponseMessage");
	node = e_soap_parameter_get_first_child_by_name (subparam, "ResponseCode");

	/* Negative cases */
	if (strcmp (e_soap_parameter_get_string_value(node), "NoError") != EWS_CONNECTION_STATUS_OK) {
		/* free memory */
		g_object_unref (response);
	}

	node = e_soap_parameter_get_first_child_by_name (subparam, "SyncState");
	new_sync_state = e_soap_parameter_get_string_value (node);
	g_print ("\n The sync state is... \n %s\n", new_sync_state);

	node = e_soap_parameter_get_first_child_by_name (subparam, "IncludesLastFolderInRange");
	if (!strcmp (e_soap_parameter_get_string_value (node), "true")) {
		/* This suggests we have received all the data and no need to make more sync
		 * hierarchy requests.
		 */
	}

	node = e_soap_parameter_get_first_child_by_name (subparam, "Changes");
	
	if (node) {
		ESoapParameter *subparam1;
		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Create");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Create")) {
			EEwsFolder *folder;

			folder = e_ews_folder_new_from_soap_parameter (subparam1);
			/* Add the folders in the "created" list of folders */
		}

		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Update");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Update")) {
			EEwsFolder *folder;

			folder = e_ews_folder_new_from_soap_parameter (subparam1);
			/* Add the folders in the "updated" list of folders */
		}

		for (subparam1 = e_soap_parameter_get_first_child_by_name (node, "Delete");
		     subparam1 != NULL;
		     subparam1 = e_soap_parameter_get_next_child_by_name (subparam1, "Delete")) {
			ESoapParameter *folder_param;
			gchar *value;

			folder_param = e_soap_parameter_get_first_child_by_name (subparam1, "FolderId");
			value = e_soap_parameter_get_property (folder_param, "Id");
			g_print("\n The deleted folder id is... %s\n", value);
			g_free (value);

//			/* Should we construct a folder for the delete types? */
//			folder = e_ews_folder_new_from_soap_parameter (subparam1);
			/* Add the folders in the "deleted" list of folders */
		}
	}

	/* free memory */
	g_object_unref (response);

	/* FIXME propogate errors */
	async_data = g_simple_async_result_get_op_res_gpointer (enode->simple);
	async_data->folders = folders;
	
	g_simple_async_result_complete_in_idle (enode->simple);
	
	found = ews_active_job_done (cnc, msg);
}


static void
e_ews_connection_dispose (GObject *object)
{
	EEwsConnection *cnc = (EEwsConnection *) object;
	EEwsConnectionPrivate *priv;
	gchar *hash_key;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	priv = cnc->priv;
	printf ("ews connection dispose \n");

	/* removed the connection from the hash table */
	if (loaded_connections_permissions != NULL) {
		hash_key = g_strdup_printf ("%s:%s@%s",
					    priv->username ? priv->username : "",
					    priv->password ? priv->password : "",
					    priv->uri ? priv->uri : "");
		g_hash_table_remove (loaded_connections_permissions, hash_key);
		if (g_hash_table_size (loaded_connections_permissions) == 0) {
			g_hash_table_destroy (loaded_connections_permissions);
			loaded_connections_permissions = NULL;
		}
		g_free (hash_key);
	}

	if (priv) {
		if (priv->soup_session) {
			g_object_unref (priv->soup_session);
			priv->soup_session = NULL;
		}

		if (priv->uri) {
			g_free (priv->uri);
			priv->uri = NULL;
		}

		if (priv->username) {
			g_free (priv->username);
			priv->username = NULL;
		}

		if (priv->password) {
			g_free (priv->password);
			priv->password = NULL;
		}

		if (priv->jobs) {
			g_slist_free (priv->jobs);
			priv->jobs = NULL;
		}

		if (priv->active_job_queue) {
			g_slist_free (priv->active_job_queue);
			priv->active_job_queue = NULL;
		}
	}

	if (parent_class->dispose)
		(* parent_class->dispose) (object);
}

static void
e_ews_connection_finalize (GObject *object)
{
	EEwsConnection *cnc = (EEwsConnection *) object;
	EEwsConnectionPrivate *priv;

	g_return_if_fail (E_IS_EWS_CONNECTION (cnc));

	priv = cnc->priv;
	printf ("ews connection finalize\n");
	/* clean up */
	g_free (priv);
	cnc->priv = NULL;

	if (parent_class->finalize)
		(* parent_class->finalize) (object);
}

static void
e_ews_connection_class_init (EEwsConnectionClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	parent_class = g_type_class_peek_parent (klass);

	object_class->dispose = e_ews_connection_dispose;
	object_class->finalize = e_ews_connection_finalize;

	klass->next_request = NULL;
	klass->shutdown = NULL;

	/**
	 * EEwsConnection::next_request
	 **/
	signals[NEXT_REQUEST] = g_signal_new (
		"next_request",
		G_OBJECT_CLASS_TYPE (klass),
		G_SIGNAL_RUN_FIRST,
		G_STRUCT_OFFSET (EEwsConnectionClass, next_request),
		NULL, NULL,
		g_cclosure_marshal_VOID__VOID,
		G_TYPE_NONE, 0);
}

static void
e_ews_connection_init (EEwsConnection *cnc)
{
	EEwsConnectionPrivate *priv;

	/* allocate internal structure */
	priv = g_new0 (EEwsConnectionPrivate, 1);
	cnc->priv = priv;

	/* create the SoupSession for this connection */
	priv->soup_session = soup_session_async_new_with_options (SOUP_SESSION_USE_NTLM, TRUE, NULL);

	g_signal_connect (cnc, "next_request", G_CALLBACK (ews_next_request), NULL);
	g_signal_connect (priv->soup_session, "authenticate", G_CALLBACK(ews_connection_authenticate), cnc);
	g_signal_connect (priv->soup_session, "request-unqueued", G_CALLBACK (ews_next_request), cnc);
}

static void 
ews_connection_authenticate	(SoupSession *sess, SoupMessage *msg,
				 SoupAuth *auth, gboolean retrying, 
				 gpointer data)
{
	EEwsConnection *cnc = data;
	
	if (retrying) {
		g_print ("Authentication failed.");
		return;
	}
	soup_auth_authenticate (auth, cnc->priv->username, cnc->priv->password);
}


/* Connection APIS */

/**
 * e_ews_connection_new 
 * @uri: Exchange server uri
 * @username: 
 * @password: 
 * @error: Currently unused, but may require in future. Can take NULL value.
 * 
 * This does not authenticate to the server. It merely stores the username and password.
 * Authentication happens when a request is made to the server.
 * 
 * Returns: EEwsConnection
 **/
EEwsConnection *
e_ews_connection_new (const gchar *uri, const gchar *username, const gchar *password, GError **error)
{
	EEwsConnection *cnc;
	gchar *hash_key;

	static GStaticMutex connecting = G_STATIC_MUTEX_INIT;

	g_static_mutex_lock (&connecting);

	/* search the connection in our hash table */
	if (loaded_connections_permissions != NULL) {
		hash_key = g_strdup_printf ("%s:%s@%s",
				username ? username : "",
				password ? password : "",
				uri);
		cnc = g_hash_table_lookup (loaded_connections_permissions, hash_key);
		g_free (hash_key);

		if (E_IS_EWS_CONNECTION (cnc)) {
			g_object_ref (cnc);
			g_static_mutex_unlock (&connecting);
			return cnc;
		}
	}
	
	/* not found, so create a new connection */
	cnc = g_object_new (E_TYPE_EWS_CONNECTION, NULL);
	
	cnc->priv->username = g_strdup (username);
	cnc->priv->password = g_strdup (password);
	cnc->priv->uri = g_strdup (uri);


	/* add the connection to the loaded_connections_permissions hash table */
	hash_key = g_strdup_printf ("%s:%s@%s",
			cnc->priv->username ? cnc->priv->username : "",
			cnc->priv->password ? cnc->priv->password : "",
			cnc->priv->uri);
	if (loaded_connections_permissions == NULL)
		loaded_connections_permissions = g_hash_table_new_full (g_str_hash, g_str_equal,
				g_free, NULL);
	g_hash_table_insert (loaded_connections_permissions, hash_key, cnc);

	/* free memory */
	g_static_mutex_unlock (&connecting);
	return cnc;

}

gchar*
e_ews_autodiscover_ws_url (const gchar *email, const gchar *password, GError **error)
{
	gchar *url;
	gchar *domain;
	gchar *asurl = NULL;
	SoupMessage *msg;
	xmlDoc *doc;
	xmlNode *node, *child;
	xmlNs *ns;
	guint status;
	xmlOutputBuffer *buf;
	EEwsConnection *cnc;

	g_return_val_if_fail (password != NULL, NULL);
	g_return_val_if_fail (email != NULL, NULL);

	domain = strchr(email, '@');
	if (!(domain && *domain)) {
		g_set_error (
			error, EWS_CONNECTION_ERROR,
			-1,
			_("Wrong email id"));
		
		return NULL;
	}
	domain++;

	url = g_strdup_printf("https://%s/autodiscover/autodiscover.xml", domain);
	cnc = e_ews_connection_new (url, email, password, NULL);

	msg = soup_message_new("GET", url);
	soup_message_headers_append (msg->request_headers,
				     "User-Agent", "libews/0.1");

	doc = xmlNewDoc((xmlChar *) "1.0");
	node = xmlNewDocNode(doc, NULL, (xmlChar *)"Autodiscover", NULL);
	xmlDocSetRootElement(doc, node);
	ns = xmlNewNs (node,
		       (xmlChar *)"http://schemas.microsoft.com/exchange/autodiscover/outlook/requestschema/2006", NULL);

	node = xmlNewChild(node, ns, (xmlChar *)"Request", NULL);
	child = xmlNewChild(node, ns, (xmlChar *)"EMailAddress",
			    (xmlChar *)email);
	child = xmlNewChild(node, ns, (xmlChar *)"AcceptableResponseSchema", 
			    (xmlChar *)"http://schemas.microsoft.com/exchange/autodiscover/outlook/responseschema/2006a");
	
	buf = xmlAllocOutputBuffer(NULL);
	xmlNodeDumpOutput(buf, doc, xmlDocGetRootElement(doc), 0, 1, NULL);
	xmlOutputBufferFlush(buf);

	soup_message_set_request(msg, "application/xml", SOUP_MEMORY_COPY,
				 (gchar *)buf->buffer->content,
				 buf->buffer->use);
				 
	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) == 1)) {
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (msg)->request_body));
		/* print request's body */
		printf ("\n The request headers");
		printf ("\n ===================");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (msg)->request_body->data, stdout);
		fputc ('\n', stdout);
	}

	status = soup_session_send_message (cnc->priv->soup_session, msg);

	xmlOutputBufferClose (buf);
	xmlFreeDoc (doc);

	if (status != 200) {
		g_set_error (
			error, EWS_CONNECTION_ERROR,
			status,
			_("Code: %d - Unexpected response from server"),
			status);
		goto failed;
	}

	if (g_getenv ("EWS_DEBUG") && (atoi (g_getenv ("EWS_DEBUG")) == 1)){
		soup_buffer_free (soup_message_body_flatten (SOUP_MESSAGE (msg)->response_body));
		/* print response body */
		printf ("\n The response headers");
		printf ("\n =====================");
		fputc ('\n', stdout);
		fputs (SOUP_MESSAGE (msg)->response_body->data, stdout);
		fputc ('\n', stdout);
	}
	
	doc = xmlReadMemory (msg->response_body->data, msg->response_body->length,
			     "autodiscover.xml", NULL, 0);
	if (!doc) {
		g_set_error (
			error, EWS_CONNECTION_ERROR,
			-1,
			_("Failed to parse autodiscover response XML"));
		goto failed;
	}
	node = xmlDocGetRootElement(doc);
	if (strcmp((char *)node->name, "Autodiscover")) {
		g_set_error (
			error, EWS_CONNECTION_ERROR,
			-1,
			_("Failed to find <Autodiscover> element\n"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "Response"))
			break;
	}
	if (!node) {
		g_set_error (
			error, EWS_CONNECTION_ERROR,
			-1,
			_("Failed to find <Response> element\n"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "Account"))
			break;
	}
	if (!node) {
		g_set_error (
			error, EWS_CONNECTION_ERROR,
			-1,
			_("Failed to find <Account> element\n"));
		goto failed;
	}
	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    !strcmp((char *)node->name, "Protocol") &&
		    (asurl = autodiscover_parse_protocol(node)))
			break;
	}
failed:
	g_object_unref (cnc);
	return asurl;
}

/* FIXME implement this as async apis
void
e_ews_connection_sync_folder_items (EEwsConnection *cnc, const gchar *sync_state, const gchar *folder_name, GCancellable *cancellable)
{
	ESoapMessage *msg;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "SyncFolderItems");
	e_soap_message_start_element (msg, "ItemShape", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", "types", "Default");
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "SyncFolderId", NULL, NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", "types", NULL, "Id", folder_name);
	e_soap_message_end_element (msg);

	if (sync_state)
		e_ews_message_write_string_parameter (msg, "SyncState", NULL, sync_state);

	 Max changes requested 
	e_ews_message_write_int_parameter (msg, "MaxChangesReturned", NULL, 100);

	 Complete the footer and print the request 
	e_ews_message_write_footer (msg);

	ews_connection_queue_request (cnc, msg, dump_response_cb, cancellable, EWS_PRIORITY_MEDIUM);
}

void
e_ews_connection_create_folder (EEwsConnection *cnc, GCancellable *cancellable)
{
	ESoapMessage *msg;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "CreateFolder");

	e_soap_message_start_element (msg, "ParentFolderId", NULL, NULL);
	e_ews_message_write_string_parameter_with_attribute (msg, "DistinguishedFolderId", "types", NULL, "Id", "msgfolderroot");
	e_soap_message_end_element (msg);

	e_soap_message_start_element (msg, "Folders", NULL, NULL);
	e_soap_message_start_element (msg, "Folder", "types", NULL);
	e_ews_message_write_string_parameter (msg, "DisplayName", "types", "TestBharath");
	e_soap_message_end_element (msg);
	e_soap_message_end_element (msg);

	e_ews_message_write_footer (msg);

	ews_connection_queue_request (cnc, msg, create_folder_response_cb, cancellable, EWS_PRIORITY_HIGH);
} */


void 
e_ews_connection_sync_folder_hierarchy_start	(EEwsConnection *cnc, 
						 gint pri, 
						 const gchar *sync_state, 
						 GAsyncReadyCallback cb, 
						 GCancellable *cancellable,
						 gpointer user_data)
{
	ESoapMessage *msg;
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	msg = e_ews_message_new_with_header (cnc->priv->uri, "SyncFolderHierarchy");
	e_soap_message_start_element (msg, "FolderShape", NULL, NULL);
	e_ews_message_write_string_parameter (msg, "BaseShape", "types", "AllProperties");
	e_soap_message_end_element (msg);

	if (sync_state)
		e_ews_message_write_string_parameter (msg, "SyncState", NULL, sync_state);

	e_ews_message_write_footer (msg);

      	simple = g_simple_async_result_new (G_OBJECT (cnc),
                                      cb,
                                      user_data,
                                      e_ews_connection_sync_folder_hierarchy_start);

	async_data = g_new0 (EwsAsyncData, 1);
	g_simple_async_result_set_op_res_gpointer (
		simple, async_data, (GDestroyNotify) async_data_free);

	ews_connection_queue_request (cnc, msg, sync_hierarchy_response_cb, pri, cancellable, simple);
}


GList * 
e_ews_connection_sync_folder_hierarchy_finish	(EEwsConnection *cnc, 
						 GAsyncResult *result, 
						 GError **error)
{
	GSimpleAsyncResult *simple;
	EwsAsyncData *async_data;

	g_return_val_if_fail (
		g_simple_async_result_is_valid (
		result, G_OBJECT (cnc), e_ews_connection_sync_folder_hierarchy_start), NULL);

	simple = G_SIMPLE_ASYNC_RESULT (result);
	async_data = g_simple_async_result_get_op_res_gpointer (simple);

	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	return async_data->folders;
}

GList *	
e_ews_connection_sync_folder_hierarchy	(EEwsConnection *cnc, 
					 gint pri, 
					 const gchar *sync_state, 
					 GCancellable *cancellable, 
					 GError **error)
{
	EwsSyncData *sync_data;
	GList *folders;

	sync_data = g_new0 (EwsSyncData, 1);
	sync_data->context = g_main_context_new ();
	sync_data->loop = g_main_loop_new (sync_data->context, FALSE);
	
	g_main_context_push_thread_default (sync_data->context);
	e_ews_connection_sync_folder_hierarchy_start	(cnc, pri, sync_state, 
							 ews_sync_reply_cb, cancellable, 
							 (gpointer) sync_data);

	g_main_loop_run (sync_data->loop);
	folders = e_ews_connection_sync_folder_hierarchy_finish	(cnc, sync_data->res, error);

	g_main_context_unref (sync_data->context);
	g_main_loop_unref (sync_data->loop);
	g_object_unref (sync_data->res);
	g_free (sync_data);

	return folders;  
}
