/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
  AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
  OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _XOPEN_SOURCE 500

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "lv2/lv2plug.in/ns/ext/event/event-helpers.h"
#include "lv2/lv2plug.in/ns/ext/event/event.h"
#include "lv2/lv2plug.in/ns/ext/uri-map/uri-map.h"

#include "slv2/slv2.h"

#include "slv2-config.h"

#ifdef SLV2_JACK_SESSION
#include <jack/session.h>
#include <glib.h>

GMutex* exit_mutex;
GCond*  exit_cond;
#endif /* SLV2_JACK_SESSION */

#define MIDI_BUFFER_SIZE 1024

enum PortType {
	CONTROL,
	AUDIO,
	EVENT
};

struct Port {
	SLV2Port           slv2_port;
	enum PortType      type;
	jack_port_t*       jack_port; /**< For audio/MIDI ports, otherwise NULL */
	float              control;   /**< For control ports, otherwise 0.0f */
	LV2_Event_Buffer*  ev_buffer; /**< For MIDI ports, otherwise NULL */
	bool               is_input;
};

/** This program's data */
struct JackHost {
	jack_client_t* jack_client;   /**< Jack client */
	SLV2Plugin     plugin;        /**< Plugin "class" (actually just a few strings) */
	SLV2Instance   instance;      /**< Plugin "instance" (loaded shared lib) */
	uint32_t       num_ports;     /**< Size of the two following arrays: */
	struct Port*   ports;         /**< Port array of size num_ports */
	SLV2Value      input_class;   /**< Input port class (URI) */
	SLV2Value      output_class;  /**< Output port class (URI) */
	SLV2Value      control_class; /**< Control port class (URI) */
	SLV2Value      audio_class;   /**< Audio port class (URI) */
	SLV2Value      event_class;   /**< Event port class (URI) */
	SLV2Value      midi_class;    /**< MIDI event class (URI) */
	SLV2Value      optional;      /**< lv2:connectionOptional port property */
};

/** URI map feature, for event types (we use only MIDI) */
#define MIDI_EVENT_ID 1
uint32_t
uri_to_id(LV2_URI_Map_Callback_Data callback_data,
          const char*               map,
          const char*               uri)
{
	/* Note a non-trivial host needs to use an actual dictionary here */
	if (!strcmp(map, LV2_EVENT_URI) && !strcmp(uri, SLV2_EVENT_CLASS_MIDI))
		return MIDI_EVENT_ID;
	else
		return 0;  /* Refuse to map ID */
}

#define NS_EXT "http://lv2plug.in/ns/ext/"

static LV2_URI_Map_Feature uri_map         = { NULL, &uri_to_id };
static const LV2_Feature   uri_map_feature = { NS_EXT "uri-map", &uri_map };

const LV2_Feature* features[2] = { &uri_map_feature, NULL };

/** Abort and exit on error */
static void
die(const char* msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

/** Creates a port and connects the plugin instance to its data location.
 *
 * For audio ports, creates a jack port and connects plugin port to buffer.
 *
 * For control ports, sets controls array to default value and connects plugin
 * port to that element.
 */
void
create_port(struct JackHost* host,
            uint32_t         port_index,
            float            default_value)
{
	struct Port* const port = &host->ports[port_index];

	port->slv2_port = slv2_plugin_get_port_by_index(host->plugin, port_index);
	port->jack_port = NULL;
	port->control   = 0.0f;
	port->ev_buffer = NULL;

	slv2_instance_connect_port(host->instance, port_index, NULL);

	/* Get the port symbol for console printing */
	SLV2Value symbol       = slv2_port_get_symbol(host->plugin, port->slv2_port);
	const char* symbol_str = slv2_value_as_string(symbol);

	enum JackPortFlags jack_flags = 0;
	if (slv2_port_is_a(host->plugin, port->slv2_port, host->input_class)) {
		jack_flags     = JackPortIsInput;
		port->is_input = true;
	} else if (slv2_port_is_a(host->plugin, port->slv2_port, host->output_class)) {
		jack_flags     = JackPortIsOutput;
		port->is_input = false;
	} else if (slv2_port_has_property(host->plugin, port->slv2_port, host->optional)) {
		slv2_instance_connect_port(host->instance, port_index, NULL);
	} else {
		die("Mandatory port has unknown type (neither input or output)");
	}

	/* Set control values */
	if (slv2_port_is_a(host->plugin, port->slv2_port, host->control_class)) {
		port->type    = CONTROL;
		port->control = isnan(default_value) ? 0.0 : default_value;
		printf("%s = %f\n", symbol_str, host->ports[port_index].control);
	} else if (slv2_port_is_a(host->plugin, port->slv2_port, host->audio_class)) {
		port->type = AUDIO;
	} else if (slv2_port_is_a(host->plugin, port->slv2_port, host->event_class)) {
		port->type = EVENT;
	}

	/* Connect the port based on its type */
	switch (port->type) {
	case CONTROL:
		slv2_instance_connect_port(host->instance, port_index, &port->control);
		break;
	case AUDIO:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str, JACK_DEFAULT_AUDIO_TYPE, jack_flags, 0);
		break;
	case EVENT:
		port->jack_port = jack_port_register(
			host->jack_client, symbol_str, JACK_DEFAULT_MIDI_TYPE, jack_flags, 0);
		port->ev_buffer = lv2_event_buffer_new(MIDI_BUFFER_SIZE, LV2_EVENT_AUDIO_STAMP);
		slv2_instance_connect_port(host->instance, port_index, port->ev_buffer);
		break;
	default:
		/* FIXME: check if port connection is optional and die if not */
		slv2_instance_connect_port(host->instance, port_index, NULL);
		fprintf(stderr, "WARNING: Unknown port type, port not connected.\n");
	}
}

/** Jack process callback. */
int
jack_process_cb(jack_nframes_t nframes, void* data)
{
	struct JackHost* const host = (struct JackHost*)data;

	/* Prepare port buffers */
	for (uint32_t p = 0; p < host->num_ports; ++p) {
		if (!host->ports[p].jack_port)
			continue;

		if (host->ports[p].type == AUDIO) {
			/* Connect plugin port directly to Jack port buffer. */
			slv2_instance_connect_port(
				host->instance, p,
				jack_port_get_buffer(host->ports[p].jack_port, nframes));

		} else if (host->ports[p].type == EVENT) {
			/* Clear Jack event port buffer. */
			lv2_event_buffer_reset(host->ports[p].ev_buffer,
			                       LV2_EVENT_AUDIO_STAMP,
			                       (uint8_t*)(host->ports[p].ev_buffer + 1));

			if (host->ports[p].is_input) {
				void* buf = jack_port_get_buffer(host->ports[p].jack_port,
				                                 nframes);

				LV2_Event_Iterator iter;
				lv2_event_begin(&iter, host->ports[p].ev_buffer);

				for (uint32_t i = 0; i < jack_midi_get_event_count(buf); ++i) {
					jack_midi_event_t ev;
					jack_midi_event_get(&ev, buf, i);
					lv2_event_write(&iter,
					                ev.time, 0,
					                MIDI_EVENT_ID, ev.size, ev.buffer);
				}
			}
		}
	}

	/* Run plugin for this cycle */
	slv2_instance_run(host->instance, nframes);

	/* Deliver MIDI output */
	for (uint32_t p = 0; p < host->num_ports; ++p) {
		if (host->ports[p].jack_port
		    && !host->ports[p].is_input
		    && host->ports[p].type == EVENT) {

			void* buf = jack_port_get_buffer(host->ports[p].jack_port,
			                                 nframes);

			jack_midi_clear_buffer(buf);

			LV2_Event_Iterator iter;
			lv2_event_begin(&iter, host->ports[p].ev_buffer);

			for (uint32_t i = 0; i < iter.buf->event_count; ++i) {
				uint8_t*   data;
				LV2_Event* ev = lv2_event_get(&iter, &data);
				jack_midi_event_write(buf, ev->frames, data, ev->size);
				lv2_event_increment(&iter);
			}
		}
	}

	return 0;
}

#ifdef SLV2_JACK_SESSION
void
jack_session_cb(jack_session_event_t* event, void* arg)
{
	struct JackHost* host = (struct JackHost*)arg;

	char cmd[256];
	snprintf(cmd, sizeof(cmd), "lv2_jack_host %s %s",
	         slv2_value_as_uri(slv2_plugin_get_uri(host->plugin)),
	         event->client_uuid);

	event->command_line = strdup(cmd);
	jack_session_reply(host->jack_client, event);

	switch (event->type) {
	case JackSessionSave:
		break;
	case JackSessionSaveAndQuit:
		g_mutex_lock(exit_mutex);
		g_cond_signal(exit_cond);
		g_mutex_unlock(exit_mutex);
		break;
	case JackSessionSaveTemplate:
		break;
	}

	jack_session_event_free(event);
}
#endif /* SLV2_JACK_SESSION */

static void
signal_handler(int ignored)
{
#ifdef SLV2_JACK_SESSION
	g_mutex_lock(exit_mutex);
	g_cond_signal(exit_cond);
	g_mutex_unlock(exit_mutex);
#endif
}

int
main(int argc, char** argv)
{
	struct JackHost host;
	host.jack_client = NULL;
	host.num_ports   = 0;
	host.ports       = NULL;

#ifdef SLV2_JACK_SESSION
	if (!g_thread_supported()) {
		g_thread_init(NULL);
	}
	exit_mutex = g_mutex_new();
	exit_cond  = g_cond_new();
#endif

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Find all installed plugins */
	SLV2World world = slv2_world_new();
	slv2_world_load_all(world);
	SLV2Plugins plugins = slv2_world_get_all_plugins(world);

	/* Set up the port classes this app supports */
	host.input_class   = slv2_value_new_uri(world, SLV2_PORT_CLASS_INPUT);
	host.output_class  = slv2_value_new_uri(world, SLV2_PORT_CLASS_OUTPUT);
	host.control_class = slv2_value_new_uri(world, SLV2_PORT_CLASS_CONTROL);
	host.audio_class   = slv2_value_new_uri(world, SLV2_PORT_CLASS_AUDIO);
	host.event_class   = slv2_value_new_uri(world, SLV2_PORT_CLASS_EVENT);
	host.midi_class    = slv2_value_new_uri(world, SLV2_EVENT_CLASS_MIDI);
	host.optional      = slv2_value_new_uri(world, SLV2_NAMESPACE_LV2
	                                        "connectionOptional");

#ifdef SLV2_JACK_SESSION
	if (argc != 2 && argc != 3) {
		fprintf(stderr, "Usage: %s PLUGIN_URI [JACK_UUID]\n", argv[0]);
#else
	if (argc != 2) {
		fprintf(stderr, "Usage: %s PLUGIN_URI\n", argv[0]);
#endif
		slv2_world_free(world);
		return EXIT_FAILURE;
	}

	const char* const plugin_uri_str = argv[1];

	printf("Plugin:    %s\n", plugin_uri_str);

	SLV2Value plugin_uri = slv2_value_new_uri(world, plugin_uri_str);
	host.plugin = slv2_plugins_get_by_uri(plugins, plugin_uri);
	slv2_value_free(plugin_uri);

	if (!host.plugin) {
		fprintf(stderr, "Failed to find plugin %s.\n", plugin_uri_str);
		slv2_world_free(world);
		return EXIT_FAILURE;
	}

	/* Get the plugin's name */
	SLV2Value   name     = slv2_plugin_get_name(host.plugin);
	const char* name_str = slv2_value_as_string(name);

	/* Truncate plugin name to suit JACK (if necessary) */
	char* jack_name = NULL;
	if (strlen(name_str) >= (unsigned)jack_client_name_size() - 1) {
		jack_name = calloc(jack_client_name_size(), sizeof(char));
		strncpy(jack_name, name_str, jack_client_name_size() - 1);
	} else {
		jack_name = strdup(name_str);
	}

	/* Connect to JACK */
	printf("JACK Name: %s\n\n", jack_name);
#ifdef SLV2_JACK_SESSION
	const char* const jack_uuid_str = (argc > 2) ? argv[2] : NULL;
	if (jack_uuid_str) {
		host.jack_client = jack_client_open(jack_name, JackSessionID, NULL,
		                                    jack_uuid_str);
	}
#endif

	if (!host.jack_client) {
		host.jack_client = jack_client_open(jack_name, JackNullOption, NULL);
	}

	free(jack_name);
	slv2_value_free(name);

	if (!host.jack_client)
		die("Failed to connect to JACK.\n");

	/* Instantiate the plugin */
	host.instance = slv2_plugin_instantiate(
		host.plugin, jack_get_sample_rate(host.jack_client), features);
	if (!host.instance)
		die("Failed to instantiate plugin.\n");

	jack_set_process_callback(host.jack_client, &jack_process_cb, (void*)(&host));
#ifdef SLV2_JACK_SESSION
	jack_set_session_callback(host.jack_client, &jack_session_cb, (void*)(&host));
#endif

	/* Create ports */
	host.num_ports = slv2_plugin_get_num_ports(host.plugin);
	host.ports     = calloc((size_t)host.num_ports, sizeof(struct Port));
	float* default_values = calloc(slv2_plugin_get_num_ports(host.plugin),
	                               sizeof(float));
	slv2_plugin_get_port_ranges_float(host.plugin, NULL, NULL, default_values);

	for (uint32_t i = 0; i < host.num_ports; ++i)
		create_port(&host, i, default_values[i]);

	free(default_values);

	/* Activate plugin and JACK */
	slv2_instance_activate(host.instance);
	jack_activate(host.jack_client);

	/* Run */
#ifdef SLV2_JACK_SESSION
	printf("\nPress Ctrl-C to quit: ");
	fflush(stdout);
	g_cond_wait(exit_cond, exit_mutex);
#else
	printf("\nPress enter to quit: ");
	fflush(stdout);
	getc(stdin);
#endif
	printf("\n");

	/* Deactivate JACK */
	jack_deactivate(host.jack_client);

	for (uint32_t i = 0; i < host.num_ports; ++i) {
		if (host.ports[i].jack_port != NULL) {
			jack_port_unregister(host.jack_client, host.ports[i].jack_port);
			host.ports[i].jack_port = NULL;
		}
		if (host.ports[i].ev_buffer != NULL) {
			free(host.ports[i].ev_buffer);
		}
	}
	jack_client_close(host.jack_client);

	/* Deactivate plugin */
	slv2_instance_deactivate(host.instance);
	slv2_instance_free(host.instance);

	/* Clean up */
	free(host.ports);
	slv2_value_free(host.input_class);
	slv2_value_free(host.output_class);
	slv2_value_free(host.control_class);
	slv2_value_free(host.audio_class);
	slv2_value_free(host.event_class);
	slv2_value_free(host.midi_class);
	slv2_value_free(host.optional);
	slv2_plugins_free(world, plugins);
	slv2_world_free(world);

#ifdef SLV2_JACK_SESSION
	g_mutex_free(exit_mutex);
	g_cond_free(exit_cond);
#endif

	return 0;
}