// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// "digitador" - headless Remote Play text-input client.
//
// Opens a Remote Play session with video, audio and controller input all
// disabled. Its ONLY purpose is to ride the ctrl channel's keyboard sub-protocol
// so you can type into PS5/PS4 text fields using your PC keyboard, freely and in
// any language, instead of the console's on-screen keyboard.
//
// It reads UTF-8 lines from stdin. Each line you type + Enter is sent to the
// currently focused text field on the console and accepted.

#include <chiaki-cli.h>

#include <chiaki/session.h>
#include <chiaki/base64.h>

#include <argp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h> // ssize_t

static char doc[] =
	"Headless text-input (\"digitador\") for PS5/PS4 Remote Play.\n"
	"Opens a video/audio/controller-less session and lets you type into the "
	"console's text fields from the PC keyboard. Type a line and press Enter to "
	"send it to the focused field; Ctrl+D to quit.";

#define ARG_KEY_HOST       'h'
#define ARG_KEY_REGISTKEY  'r'
#define ARG_KEY_MORNING    'm'
#define ARG_KEY_ACCOUNTID  'a'
#define ARG_KEY_CREDS      'c'
#define ARG_KEY_PS4        '4'
#define ARG_KEY_PS5        '5'

static struct argp_option options[] = {
	{ "creds",      ARG_KEY_CREDS,     "File",      0, "Load host/registkey/morning/accountid/console from a creds file (written by 'register')", 0 },
	{ "host",       ARG_KEY_HOST,      "Host",      0, "Console IP address", 0 },
	{ "registkey",  ARG_KEY_REGISTKEY, "RegistKey", 0, "Remote Play registration key (the rp_regist_key, plaintext)", 0 },
	{ "morning",    ARG_KEY_MORNING,   "Morning",   0, "RP key (rp_key) as 32 hex chars = 16 bytes", 0 },
	{ "accountid",  ARG_KEY_ACCOUNTID, "AccountID", 0, "PSN Account ID, base64 (8 bytes encoded)", 0 },
	{ "ps4",        ARG_KEY_PS4,       NULL,        0, "PlayStation 4", 0 },
	{ "ps5",        ARG_KEY_PS5,       NULL,        0, "PlayStation 5 (default)", 0 },
	{ 0 }
};

typedef struct arguments
{
	const char *host;
	const char *registkey;
	const char *morning;
	const char *accountid;
	const char *creds;
	bool ps5;
	bool ps5_set;
} Arguments;

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	Arguments *arguments = state->input;
	switch(key)
	{
		case ARG_KEY_HOST:      arguments->host = arg; break;
		case ARG_KEY_REGISTKEY: arguments->registkey = arg; break;
		case ARG_KEY_MORNING:   arguments->morning = arg; break;
		case ARG_KEY_ACCOUNTID: arguments->accountid = arg; break;
		case ARG_KEY_CREDS:     arguments->creds = arg; break;
		case ARG_KEY_PS4:       arguments->ps5 = false; arguments->ps5_set = true; break;
		case ARG_KEY_PS5:       arguments->ps5 = true; arguments->ps5_set = true; break;
		case ARGP_KEY_ARG:      argp_usage(state); break;
		default:                return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

// Fill any field not given on the command line from a key=value creds file.
// Explicit CLI flags always win over the file. Returns 0 on success.
static int load_creds_file(const char *path, Arguments *a)
{
	FILE *f = fopen(path, "r");
	if(!f)
	{
		fprintf(stderr, "Nao consegui abrir o arquivo de credenciais: %s\n", path);
		return -1;
	}
	char line[512];
	while(fgets(line, sizeof(line), f))
	{
		char *nl = strpbrk(line, "\r\n");
		if(nl)
			*nl = '\0';
		if(line[0] == '#' || line[0] == '\0')
			continue;
		char *eq = strchr(line, '=');
		if(!eq)
			continue;
		*eq = '\0';
		const char *key = line;
		const char *val = eq + 1;
		if(strcmp(key, "host") == 0 && !a->host)
			a->host = strdup(val);
		else if(strcmp(key, "registkey") == 0 && !a->registkey)
			a->registkey = strdup(val);
		else if(strcmp(key, "morning") == 0 && !a->morning)
			a->morning = strdup(val);
		else if(strcmp(key, "accountid") == 0 && !a->accountid)
			a->accountid = strdup(val);
		else if(strcmp(key, "console") == 0 && !a->ps5_set)
			a->ps5 = (strcmp(val, "ps4") != 0);
	}
	fclose(f);
	return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

// ---- shared state between the session thread (event_cb) and the stdin loop ----
static ChiakiSession *g_session = NULL;
static volatile bool g_connected = false;
static volatile bool g_keyboard_open = false;
static volatile bool g_quit = false;

static void event_cb(ChiakiEvent *event, void *user)
{
	(void)user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			g_connected = true;
			fprintf(stderr, "[digitador] Conectado ao console.\n");
			fprintf(stderr, "[digitador] Abra um campo de texto no jogo/app do console e comece a digitar aqui.\n");
			break;
		case CHIAKI_EVENT_KEYBOARD_OPEN:
			g_keyboard_open = true;
			fprintf(stderr, "[digitador] >>> Campo de texto ABERTO no console. Digite a linha e pressione Enter.\n");
			break;
		case CHIAKI_EVENT_KEYBOARD_TEXT_CHANGE:
			if(event->keyboard.text_str)
				fprintf(stderr, "[digitador] (console) texto atual: \"%s\"\n", event->keyboard.text_str);
			break;
		case CHIAKI_EVENT_KEYBOARD_REMOTE_CLOSE:
			g_keyboard_open = false;
			fprintf(stderr, "[digitador] <<< Campo de texto fechado pelo console.\n");
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			fprintf(stderr, "[digitador] O console pediu PIN de login. Este modo headless nao trata PIN;\n"
			                "            registre o console antes (via chiaki-ng ou o subcomando register).\n");
			break;
		case CHIAKI_EVENT_QUIT:
			g_quit = true;
			fprintf(stderr, "[digitador] Sessao encerrada: %s\n",
				event->quit.reason_str ? event->quit.reason_str
				                       : chiaki_quit_reason_string(event->quit.reason));
			break;
		default:
			break;
	}
}

// Discard any video the console still pushes (should be none with AV disabled).
static bool video_sample_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost,
                            bool frame_recovered, void *user)
{
	(void)buf; (void)buf_size; (void)frames_lost; (void)frame_recovered; (void)user;
	return true;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
	if(!hex || strlen(hex) != out_len * 2)
		return -1;
	for(size_t i = 0; i < out_len; i++)
	{
		unsigned int v;
		if(sscanf(hex + 2 * i, "%2x", &v) != 1)
			return -1;
		out[i] = (uint8_t)v;
	}
	return 0;
}

CHIAKI_EXPORT int chiaki_cli_cmd_type(ChiakiLog *log, int argc, char *argv[])
{
	Arguments arguments = { 0 };
	arguments.ps5 = true;
	if(argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &arguments) != 0)
		return 1;

	if(arguments.creds && load_creds_file(arguments.creds, &arguments) != 0)
		return 1;

	if(!arguments.host)      { fprintf(stderr, "Faltou --host.\n"); return 1; }
	if(!arguments.registkey) { fprintf(stderr, "Faltou --registkey.\n"); return 1; }
	if(!arguments.morning)   { fprintf(stderr, "Faltou --morning.\n"); return 1; }
	if(!arguments.accountid) { fprintf(stderr, "Faltou --accountid.\n"); return 1; }

	ChiakiConnectInfo connect_info;
	memset(&connect_info, 0, sizeof(connect_info));
	connect_info.ps5 = arguments.ps5;
	connect_info.host = arguments.host;

	// regist_key: the rp_regist_key buffer, 16 bytes, zero-padded.
	if(strlen(arguments.registkey) > sizeof(connect_info.regist_key))
	{
		fprintf(stderr, "registkey longo demais (max %zu bytes).\n", sizeof(connect_info.regist_key));
		return 1;
	}
	memset(connect_info.regist_key, 0, sizeof(connect_info.regist_key));
	memcpy(connect_info.regist_key, arguments.registkey, strlen(arguments.registkey));

	// morning: rp_key, 16 raw bytes, given as 32 hex chars.
	if(hex_to_bytes(arguments.morning, connect_info.morning, sizeof(connect_info.morning)) != 0)
	{
		fprintf(stderr, "morning invalido: precisa de exatamente 32 caracteres hex.\n");
		return 1;
	}

	// psn_account_id: 8 bytes, base64-encoded.
	uint8_t account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t account_id_size = sizeof(account_id);
	if(chiaki_base64_decode(arguments.accountid, strlen(arguments.accountid),
	                        account_id, &account_id_size) != CHIAKI_ERR_SUCCESS
	   || account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		fprintf(stderr, "accountid invalido: precisa decodificar para %d bytes em base64.\n",
			CHIAKI_PSN_ACCOUNT_ID_SIZE);
		return 1;
	}
	memcpy(connect_info.psn_account_id, account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE);

	// The whole point: keyboard channel ON, everything else OFF.
	connect_info.enable_keyboard = true;
	connect_info.enable_dualsense = false;
	connect_info.audio_video_disabled = CHIAKI_AUDIO_VIDEO_DISABLED;
	connect_info.video_profile_auto_downgrade = true;
	chiaki_connect_video_profile_preset(&connect_info.video_profile,
		CHIAKI_VIDEO_RESOLUTION_PRESET_360p, CHIAKI_VIDEO_FPS_PRESET_30);

	ChiakiSession session;
	ChiakiErrorCode err = chiaki_session_init(&session, &connect_info, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_session_init falhou: %s\n", chiaki_error_string(err));
		return 1;
	}
	g_session = &session;
	chiaki_session_set_event_cb(&session, event_cb, NULL);
	chiaki_session_set_video_sample_cb(&session, video_sample_cb, NULL);

	err = chiaki_session_start(&session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_session_start falhou: %s\n", chiaki_error_string(err));
		chiaki_session_fini(&session);
		return 1;
	}

	fprintf(stderr, "[digitador] Conectando a %s (%s)...\n",
		arguments.host, arguments.ps5 ? "PS5" : "PS4");

	// Main loop: read UTF-8 lines from stdin and push each to the focused field.
	char *line = NULL;
	size_t cap = 0;
	while(!g_quit)
	{
		ssize_t n = getline(&line, &cap, stdin);
		if(n < 0) // EOF (Ctrl+D) or error
			break;
		if(g_quit)
			break;
		// strip trailing newline(s)
		while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';

		if(!g_connected)
		{
			fprintf(stderr, "[digitador] Ainda nao conectado; aguarde a mensagem de conexao.\n");
			continue;
		}
		if(!g_keyboard_open)
			fprintf(stderr, "[digitador] Aviso: nenhum campo aberto no console; enviando mesmo assim.\n");

		err = chiaki_session_keyboard_set_text(&session, line);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			fprintf(stderr, "[digitador] set_text falhou: %s\n", chiaki_error_string(err));
			continue;
		}
		err = chiaki_session_keyboard_accept(&session);
		if(err != CHIAKI_ERR_SUCCESS)
			fprintf(stderr, "[digitador] accept falhou: %s\n", chiaki_error_string(err));
		else
			fprintf(stderr, "[digitador] Enviado: \"%s\"\n", line);
	}
	free(line);

	chiaki_session_stop(&session);
	chiaki_session_join(&session);
	chiaki_session_fini(&session);
	g_session = NULL;
	return 0;
}
