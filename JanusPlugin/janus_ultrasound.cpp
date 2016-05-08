/*!
 * Heavily based on Janus Streaming Plugin by Lorenzo Miniero <lorenzo@meetecho.com>
 * see https://janus.conf.meetecho.com/
*/

extern "C" {
#include <janus/debug.h>
#include <janus/apierror.h>
#include <janus/config.h>
#include <janus/mutex.h>
#include <janus/rtp.h>
#include <janus/rtcp.h>
#include <janus/record.h>
#include <janus/utils.h>
#include <janus/plugin.h>
}

#include <errno.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <boost/format.hpp>
#include <USPipelineInterface/UltrasoundImagePipeline.h>

#include <json.hpp>
using json = nlohmann::json;

#include "plugin_hooks.h"
#include "janus_ultrasound.h"
#include "RTPSource.h"
#include "rtp_functions.h"
#include "JanusUltrasoundSessionManager.h"
#include "auth/Authenticator.h"
#include "auth/DummyAuthenticator.h"
#include "auth/SimpleAuthenticator.h"

#include <mutex>
#include <queue>

//TODO: move static functions into a class and make below members

static JanusUltrasoundSessionManager* session_manager;
static Authenticator* authenticator;

GHashTable *mountpoints;
janus_mutex mountpoints_mutex;

static GHashTable *sessions;
static janus_mutex sessions_mutex;

std::queue<Message*> messages;
std::mutex messages_mutex;
std::thread* handler_thread;


/* Plugin creator */
janus_plugin *create(void) {
    JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_ULTRASOUND_NAME);
    return &janus_ultrasound_plugin;
}

/* Plugin implementation */
int janus_ultrasound_init(janus_callbacks *callback, const char *config_path) {
    if(g_atomic_int_get(&stopping)) {
        /* Still stopping from before */
        return -1;
    }

    mountpoints = g_hash_table_new(NULL, NULL);
    janus_mutex_init(&mountpoints_mutex);

    sessions = g_hash_table_new(NULL, NULL);
    janus_mutex_init(&sessions_mutex);
    /* This is the callback we'll need to invoke to contact the gateway */
    gateway = callback;
    g_atomic_int_set(&initialized, 1);


    /* Launch the thread that will handle incoming messages */
    handler_thread = new std::thread(janus_ultrasound_handler);

    session_manager = new JanusUltrasoundSessionManager(gateway);
    authenticator = new SimpleAuthenticator();

    JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_ULTRASOUND_NAME);

    return 0;
}

void janus_ultrasound_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

    if(handler_thread->joinable()) {
        handler_thread->join();
        handler_thread = nullptr;
    }

	/* Remove all mountpoints */
	janus_mutex_unlock(&mountpoints_mutex);
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, mountpoints);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
        delete (RTPSource*)value;
	}
	janus_mutex_unlock(&mountpoints_mutex);

	/* FIXME We should destroy the sessions cleanly */
	usleep(500000);
	janus_mutex_lock(&mountpoints_mutex);
	g_hash_table_destroy(mountpoints);
	janus_mutex_unlock(&mountpoints_mutex);
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	janus_mutex_unlock(&sessions_mutex);

    while(!messages.empty()) messages.pop();

	sessions = NULL;


	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);


    delete session_manager;

	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_ULTRASOUND_NAME);
}

void janus_ultrasound_create_session(janus_plugin_session *handle, int *error) {

	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	janus_ultrasound_session *session = (janus_ultrasound_session *)g_malloc0(sizeof(janus_ultrasound_session));
	if(session == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory error!\n");
		*error = -2;
		return;
	}
	session->handle = handle;
	session->mountpoint = NULL;	/* This will happen later */
    session->started = FALSE;	/* This will happen later */
	session->destroyed = 0;
    session->gateway = gateway;
	g_atomic_int_set(&session->hangingup, 0);
	handle->plugin_handle = session;
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
    janus_mutex_unlock(&sessions_mutex);

    session_manager->newSession(handle);

	return;
}

void janus_ultrasound_destroy_session(janus_plugin_session *handle, int *error) {

	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	

    session_manager->destroySession(handle);

	janus_ultrasound_session *session = (janus_ultrasound_session *)handle->plugin_handle; 
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
	JANUS_LOG(LOG_VERB, "Removing ultrasound session...\n");
	if(session->mountpoint) {
		janus_mutex_lock(&session->mountpoint->mutex);
        session->mountpoint->listeners = g_list_remove_all(session->mountpoint->listeners, session);
        janus_mutex_unlock(&session->mountpoint->mutex);

        delete session->mountpoint;
	}
	janus_mutex_lock(&sessions_mutex);
	if(!session->destroyed) {
        printf("Freeing old Streaming session\n");
        session->handle = NULL;
        g_free(session);
		g_hash_table_remove(sessions, handle);
	}
	janus_mutex_unlock(&sessions_mutex);


	return;
}

char *janus_ultrasound_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}	
	janus_ultrasound_session *session = (janus_ultrasound_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* What is this user watching, if anything? */
    json info;
    info["state"] = session->mountpoint ? "watching" : "idle";
	if(session->mountpoint) {
        info["mountpoint_id"] = session->mountpoint->id;
        info["mountpoint_name"] = session->mountpoint->name ? session->mountpoint->name : nullptr;
	}
    info["destroyed"] = session->destroyed;
    std::string info_text = info.dump(info);
    return info_text.c_str();
}

janus_plugin_result* Plugin::onMessage(janus_plugin_session *handle, char *transaction,
                                       char *message, char *sdp_type, char *sdp)
{

    if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
        return janus_plugin_result_new(JANUS_PLUGIN_ERROR,
               g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized");
    }

    if(message == NULL || handle ->plugin_handle == NULL ||
      ((janus_ultrasound_session *)handle->plugin_handle)->destroyed)
    {
        return Plugin::handleMessageError(handle, transaction, message, NULL, sdp_type, sdp, "Empty message");
    }

    json root;
    try {
        root = json::parse(message);
    } catch (std::invalid_argument e) {
        return Plugin::handleMessageError(handle, transaction, message, root, sdp_type, sdp, "JSON Error");
    }

    try {
        std::string request = root["request"];
    } catch (std::domain_error e) {
        return Plugin::handleMessageError(handle, transaction, message, root, sdp_type, sdp, "Missing element (request)");
    }

    Plugin::addMessageToQueue(handle, transaction, root, sdp_type, sdp);

    return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL);
}


void Plugin::addMessageToQueue(janus_plugin_session *handle, char *transaction, json root, char *sdp_type, char *sdp) {
    Message *msg = new Message(handle, transaction, root, sdp, sdp_type);

    std::unique_lock<std::mutex> lock(messages_mutex);
    // Lock
    messages.push(msg);
    // Unlock
}


/* Thread to handle incoming messages */
void janus_ultrasound_handler() {

    while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {

        Message *msg;
        {
            std::unique_lock<std::mutex> lock(messages_mutex);
            // Lock
            if (messages.empty()) continue;
            msg = messages.front();
            messages.pop();
            // Unlock
        }

        // Ignore empty messages
        if (msg->message == NULL) {
            continue;
        }

        // Check session is still alive
        janus_ultrasound_session* session = (janus_ultrasound_session*) msg->handle->plugin_handle;
        if(session->destroyed) {
            delete msg;
            continue;
        }

        if(msg->message["request"] == "watch") {
            Plugin::handleMessageWatch(session, msg);
        } else if(msg->message["request"] == "ready") {
            Plugin::handleMessageReady(session, msg);
        } else if(msg->message["request"] == "start") {
            Plugin::handleMessageStart(session, msg);
        } else if(msg->message["request"] == "stop") {
            Plugin::handleMessageStop(session, msg);
        }   else {
            JANUS_LOG(LOG_VERB, "Unknown request '%s'\n", msg->message["request"]);

            json event;
            event["ultrasound"] = "event";
            event["error"] = "Unknown Request";

            std::string event_str = event.dump();

            gateway->push_event(msg->handle, &janus_ultrasound_plugin,
                                msg->transaction, event_str.c_str(), NULL, NULL);
            delete msg;
        }
    }
    return NULL;
}


void Plugin::handleMessageWatch(janus_ultrasound_session *session, Message* msg) {

    /* RTP live source (e.g., from gstreamer/ffmpeg/vlc/etc.) */
    char* desc = "ULTRASOUND";
    int pin = 0;
    int vport = session_manager->getSessionPort(msg->handle);
    int vcodec = 100;
    char* vrtpmap = "VP8/90000";
    gboolean is_private = false;

    if (!authenticator->isValid(msg->message["auth"])) {
        json event;
        event["ultrasound"] = "event";
        event["error"] = "Authentication failed";

        std::string event_str = event.dump();
        gateway->push_event(msg->handle, &janus_ultrasound_plugin, msg->transaction,
                            event_str.c_str(), NULL, NULL);
        delete msg;
        return;
    }

    RTPSource* mp = janus_ultrasound_create_rtp_source(
        1,
        desc,
        vport,
        vcodec,
        vrtpmap
    );

    session->stopping = FALSE;
    session->mountpoint = mp;

    /* TODO Check if user is already watching a stream, if the video is active, etc. */
    janus_mutex_lock(&mp->mutex);
    mp->listeners = g_list_append(mp->listeners, session);
    janus_mutex_unlock(&mp->mutex);
    char* sdp_type = "offer";	/* We're always going to do the offer ourselves, never answer */

    char* sdp = mp->createSDP();

    json result;
    result["status"] = "preparing";

    Plugin::sendPostMessageEvent(result, msg, sdp, sdp_type);
}


void Plugin::handleMessageStart(janus_ultrasound_session *session, Message* msg) {
    JANUS_LOG(LOG_VERB, "Starting the ultrasound\n");
    json result;
    /* We wait for the setup_media event to start: on the other hand, it may have already arrived */
    result["status"] = session->started ? "started" : "starting";
    Plugin::sendPostMessageEvent(result, msg, NULL, NULL);
}


void Plugin::handleMessageStop(janus_ultrasound_session *session, Message *msg) {
    if(session->stopping || !session->started) {
        delete msg;
        return;
    }
    JANUS_LOG(LOG_VERB, "Stopping the ultrasound\n");
    session->stopping = TRUE;
    session->started = FALSE;

    json result;
    result["status"] = "stopping";
    if(session->mountpoint) {
        janus_mutex_lock(&session->mountpoint->mutex);
        JANUS_LOG(LOG_VERB, "  -- Removing the session from the mountpoint listeners\n");
        if(g_list_find(session->mountpoint->listeners, session) != NULL) {
            JANUS_LOG(LOG_VERB, "  -- -- Found!\n");
        }
        session->mountpoint->listeners = g_list_remove_all(session->mountpoint->listeners, session);
        janus_mutex_unlock(&session->mountpoint->mutex);
    }
    session->mountpoint = NULL;
    /* Tell the core to tear down the PeerConnection, hangup_media will do the rest */
    gateway->close_pc(session->handle);
    Plugin::sendPostMessageEvent(result, msg, NULL, NULL);
}

void Plugin::handleMessageReady(janus_ultrasound_session *session, Message* msg) {
    session_manager->onSessionReady(msg->handle);
}

janus_plugin_result* Plugin::handleMessageError(janus_plugin_session *handle, char *transaction, char* message,
                                        json root, char *sdp_type, char *sdp, char* error)
{
    free(transaction);
    free(message);
    free(sdp_type);
    free(sdp);

    /* Prepare JSON error event */
    json event;
    event["ultrasound"] = "event";
    event["error"] = error;


    std::string event_str = event.dump();
    return janus_plugin_result_new(JANUS_PLUGIN_OK, event_str.c_str());
}

void Plugin::sendPostMessageEvent(json result, Message* msg, char* sdp, char* sdp_type)
{
    /* Prepare JSON event */
    json event;
    event["ultrasound"] = "event";
    if(result != NULL)
        event["result"] = result;

    std::string event_str = event.dump();
    gateway->push_event(msg->handle, &janus_ultrasound_plugin, msg->transaction,
                        event_str.c_str(), sdp_type, sdp);

    if (sdp) free(sdp);
    delete msg;
}

void janus_ultrasound_message_free(Message *msg) {
    delete msg;
}

void Plugin::incomingData(janus_plugin_session *handle, char* buffer, int length) {
    // Check plugin health
    if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) ||
        !g_atomic_int_get(&initialized) || gateway == NULL)
    {
        return;
    }

    // Check session health
    janus_ultrasound_session *session = (janus_ultrasound_session *)handle->plugin_handle;
    if (session == NULL || session->destroyed) {
        JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
        return;
    }

    // Check message
    if (buffer == NULL || length <= 0) {
        return;
    }

    // Pass on null-terminated string
    char* msg = (char*) malloc(length+1);
    memcpy(msg, buffer, length);
    msg[length] = '\0';

    session_manager->onDataReceived(handle, msg);

    free(msg);
}

void janus_ultrasound_setup_media(janus_plugin_session *handle) {

    if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
    janus_ultrasound_session* session = (janus_ultrasound_session*) handle->plugin_handle;
    if (!session || session->destroyed) {
		return;
	}

	g_atomic_int_set(&session->hangingup, 0);
	/* We only start ultrasound towards this user when we get this event */
	session->context.v_last_ssrc = 0;
	session->context.v_last_ts = 0;
	session->context.v_base_ts = 0;
	session->context.v_base_ts_prev = 0;
	session->context.v_last_seq = 0;
	session->context.v_base_seq = 0;
	session->context.v_base_seq_prev = 0;

    session->started = TRUE;
    /* Prepare JSON event */
    json event;
    event["ultrasound"] = "event";
    event["result"]["status"] = "started";

    std::string event_str = event.dump();
    gateway->push_event(handle, &janus_ultrasound_plugin, NULL, event_str.c_str(), NULL, NULL);
}


void janus_ultrasound_hangup_media(janus_plugin_session *handle) {
    if (g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
        return;
    janus_ultrasound_session *session = (janus_ultrasound_session *)handle->plugin_handle;
    if (!session || session->destroyed || g_atomic_int_add(&session->hangingup, 1)) {
        JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
        return;
    }

    /* Simulate a "stop" coming from the browser */
    Plugin::addMessageToQueue(handle, NULL, json::parse("{\"request\":\"stop\"}"), NULL, NULL);
}


void janus_ultrasound_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
    if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* FIXME We don't care about what the browser sends us, we're sendonly */
}

void janus_ultrasound_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
    if (handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* We might interested in the available bandwidth that the user advertizes */
	uint64_t bw = janus_rtcp_get_remb(buf, len);
	if(bw > 0) {
		JANUS_LOG(LOG_HUGE, "REMB for this PeerConnection: %"SCNu64"\n", bw);
		/* TODO Use this somehow (e.g., notification towards application?) */
	}
	/* FIXME Maybe we should care about RTCP, but not now */
}


RTPSource* janus_ultrasound_create_rtp_source(uint64_t id, char *name, uint16_t vport,
                                              uint8_t vcodec, char *vrtpmap)
{
	janus_mutex_lock(&mountpoints_mutex);

    int video_fd = create_fd(vport, INADDR_ANY, "Video", "video", name);
    if(video_fd < 0) {
        JANUS_LOG(LOG_ERR, "Can't bind to port %d for video...\n", vport);
        janus_mutex_unlock(&mountpoints_mutex);
        return NULL;
    }

	/* Create the mountpoint */

    RTPSource* rtp_source = new RTPSource(id, strdup(name), vport, video_fd, vcodec, strdup(vrtpmap));

    g_hash_table_insert(mountpoints, GINT_TO_POINTER(rtp_source->id), rtp_source);
    janus_mutex_unlock(&mountpoints_mutex);
    GError *error = NULL;
    g_thread_try_new(rtp_source->name, &relay_rtp_thread, rtp_source, &error);

    return rtp_source;
}

/* C Function Wrappers */

void janus_ultrasound_incoming_data(janus_plugin_session *handle, char* buffer, int length)
{
    Plugin::incomingData(handle, buffer, length);
}

janus_plugin_result* janus_ultrasound_handle_message(janus_plugin_session *handle, char *transaction,
                                                     char *message, char *sdp_type, char *sdp)
{
    return Plugin::onMessage(handle, transaction, message, sdp_type, sdp);
}


Message::Message(janus_plugin_session* handle, char *transaction, json message, char *sdp, char *sdp_type) {
    this->handle = handle;
    this->transaction = transaction;
    this->message = message;
    this->sdp = sdp;
    this->sdp_type = sdp_type;
}

Message::~Message() {
    if (transaction) free(transaction);
    if (sdp_type) free(sdp_type);
    if (sdp) free(sdp);
}
