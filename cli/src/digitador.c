// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// digitador - standalone text-input client for PS5/PS4 Remote Play.
//
// Self-contained executable (uses getopt_long, no argp) so it builds the same
// on Windows/mingw and Linux. Two subcommands:
//
//   digitador register --host IP --accountid B64 --pin 12345678 [--out creds.txt] [--ps4]
//   digitador type     [--creds creds.txt] | (--host IP --registkey K --morning HEX --accountid B64) [--ps4]
//
// "type" opens a Remote Play session with video/audio/controller disabled and
// rides the keyboard channel: each UTF-8 line you type + Enter goes to the
// focused text field on the console. Ctrl+D quits. Image stays on the console TV.

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/base64.h>
#include <chiaki/thread.h>
#include <chiaki/log.h>

#include <getopt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// ---------------------------------------------------------------- helpers ----

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

// ------------------------------------------------------------- type command ----

static volatile bool g_connected = false;
static volatile bool g_keyboard_open = false;
static volatile bool g_quit = false;

static void type_event_cb(ChiakiEvent *event, void *user)
{
	(void)user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			g_connected = true;
			fprintf(stderr, "[digitador] Conectado ao console.\n");
			fprintf(stderr, "[digitador] Abra um campo de texto no console e digite aqui.\n");
			break;
		case CHIAKI_EVENT_KEYBOARD_OPEN:
			g_keyboard_open = true;
			fprintf(stderr, "[digitador] >>> Campo de texto ABERTO. Digite a linha e Enter.\n");
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
			fprintf(stderr, "[digitador] O console pediu PIN de login; registre antes com 'digitador register'.\n");
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

static bool type_video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost,
                          bool frame_recovered, void *user)
{
	(void)buf; (void)buf_size; (void)frames_lost; (void)frame_recovered; (void)user;
	return true;
}

typedef struct
{
	char *host;
	char *registkey;
	char *morning;
	char *accountid;
	bool ps5;
	bool ps5_set;
} TypeArgs;

static int load_creds_file(const char *path, TypeArgs *a)
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
		const char *key = line, *val = eq + 1;
		if(strcmp(key, "host") == 0 && !a->host)
			a->host = strdup(val);
		else if(strcmp(key, "registkey") == 0 && !a->registkey)
			a->registkey = strdup(val);
		else if(strcmp(key, "morning") == 0 && !a->morning)
			a->morning = strdup(val);
		else if(strcmp(key, "accountid") == 0 && !a->accountid)
			a->accountid = strdup(val);
		else if(strcmp(key, "console") == 0 && !a->ps5_set)
		{
			a->ps5 = (strcmp(val, "ps4") != 0);
			a->ps5_set = true;
		}
	}
	fclose(f);
	return 0;
}

static int cmd_type(ChiakiLog *log, int argc, char **argv)
{
	TypeArgs a;
	memset(&a, 0, sizeof(a));
	a.ps5 = true;
	const char *creds = NULL;

	static struct option opts[] = {
		{ "creds",     required_argument, 0, 'c' },
		{ "host",      required_argument, 0, 'h' },
		{ "registkey", required_argument, 0, 'r' },
		{ "morning",   required_argument, 0, 'm' },
		{ "accountid", required_argument, 0, 'a' },
		{ "ps4",       no_argument,       0, '4' },
		{ "ps5",       no_argument,       0, '5' },
		{ 0, 0, 0, 0 }
	};
	optind = 1;
	int c;
	while((c = getopt_long(argc, argv, "c:h:r:m:a:45", opts, NULL)) != -1)
	{
		switch(c)
		{
			case 'c': creds = optarg; break;
			case 'h': a.host = strdup(optarg); break;
			case 'r': a.registkey = strdup(optarg); break;
			case 'm': a.morning = strdup(optarg); break;
			case 'a': a.accountid = strdup(optarg); break;
			case '4': a.ps5 = false; a.ps5_set = true; break;
			case '5': a.ps5 = true; a.ps5_set = true; break;
			default:
				fprintf(stderr, "Uso: digitador type [--creds arquivo] | "
				                "(--host IP --registkey K --morning HEX --accountid B64) [--ps4|--ps5]\n");
				return 1;
		}
	}

	if(creds && load_creds_file(creds, &a) != 0)
		return 1;

	if(!a.host)      { fprintf(stderr, "Faltou --host (ou --creds).\n"); return 1; }
	if(!a.registkey) { fprintf(stderr, "Faltou --registkey (ou --creds).\n"); return 1; }
	if(!a.morning)   { fprintf(stderr, "Faltou --morning (ou --creds).\n"); return 1; }
	if(!a.accountid) { fprintf(stderr, "Faltou --accountid (ou --creds).\n"); return 1; }

	ChiakiConnectInfo ci;
	memset(&ci, 0, sizeof(ci));
	ci.ps5 = a.ps5;
	ci.host = a.host;

	if(strlen(a.registkey) > sizeof(ci.regist_key))
	{
		fprintf(stderr, "registkey longo demais (max %zu).\n", sizeof(ci.regist_key));
		return 1;
	}
	memset(ci.regist_key, 0, sizeof(ci.regist_key));
	memcpy(ci.regist_key, a.registkey, strlen(a.registkey));

	if(hex_to_bytes(a.morning, ci.morning, sizeof(ci.morning)) != 0)
	{
		fprintf(stderr, "morning invalido: precisa de 32 caracteres hex.\n");
		return 1;
	}

	uint8_t account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t account_id_size = sizeof(account_id);
	if(chiaki_base64_decode(a.accountid, strlen(a.accountid), account_id, &account_id_size)
	       != CHIAKI_ERR_SUCCESS
	   || account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		fprintf(stderr, "accountid invalido: precisa decodificar para %d bytes em base64.\n",
			CHIAKI_PSN_ACCOUNT_ID_SIZE);
		return 1;
	}
	memcpy(ci.psn_account_id, account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE);

	ci.enable_keyboard = true;
	ci.enable_dualsense = false;
	ci.audio_video_disabled = CHIAKI_AUDIO_VIDEO_DISABLED;
	ci.video_profile_auto_downgrade = true;
	chiaki_connect_video_profile_preset(&ci.video_profile,
		CHIAKI_VIDEO_RESOLUTION_PRESET_360p, CHIAKI_VIDEO_FPS_PRESET_30);

	ChiakiSession session;
	ChiakiErrorCode err = chiaki_session_init(&session, &ci, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_session_init falhou: %s\n", chiaki_error_string(err));
		return 1;
	}
	chiaki_session_set_event_cb(&session, type_event_cb, NULL);
	chiaki_session_set_video_sample_cb(&session, type_video_cb, NULL);

	err = chiaki_session_start(&session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_session_start falhou: %s\n", chiaki_error_string(err));
		chiaki_session_fini(&session);
		return 1;
	}

	fprintf(stderr, "[digitador] Conectando a %s (%s)...\n", a.host, a.ps5 ? "PS5" : "PS4");

	char *line = NULL;
	size_t cap = 0;
	while(!g_quit)
	{
		ssize_t n = getline(&line, &cap, stdin);
		if(n < 0)
			break;
		if(g_quit)
			break;
		while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = '\0';

		if(!g_connected)
		{
			fprintf(stderr, "[digitador] Ainda nao conectado; aguarde.\n");
			continue;
		}
		if(!g_keyboard_open)
			fprintf(stderr, "[digitador] Aviso: nenhum campo aberto; enviando mesmo assim.\n");

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
	return 0;
}

// --------------------------------------------------------- register command ----

static ChiakiMutex g_rmutex;
static ChiakiCond g_rcond;
static bool g_rdone = false;
static bool g_rsuccess = false;
static ChiakiRegisteredHost g_rhost;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	(void)user;
	chiaki_mutex_lock(&g_rmutex);
	if(event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
	{
		g_rsuccess = true;
		g_rhost = *event->registered_host;
	}
	g_rdone = true;
	chiaki_cond_signal(&g_rcond);
	chiaki_mutex_unlock(&g_rmutex);
}

static int cmd_register(ChiakiLog *log, int argc, char **argv)
{
	const char *host = NULL, *accountid = NULL, *pin = NULL, *out = NULL;
	bool ps5 = true;

	static struct option opts[] = {
		{ "host",      required_argument, 0, 'h' },
		{ "accountid", required_argument, 0, 'a' },
		{ "pin",       required_argument, 0, 'p' },
		{ "out",       required_argument, 0, 'o' },
		{ "ps4",       no_argument,       0, '4' },
		{ "ps5",       no_argument,       0, '5' },
		{ 0, 0, 0, 0 }
	};
	optind = 1;
	int c;
	while((c = getopt_long(argc, argv, "h:a:p:o:45", opts, NULL)) != -1)
	{
		switch(c)
		{
			case 'h': host = optarg; break;
			case 'a': accountid = optarg; break;
			case 'p': pin = optarg; break;
			case 'o': out = optarg; break;
			case '4': ps5 = false; break;
			case '5': ps5 = true; break;
			default:
				fprintf(stderr, "Uso: digitador register --host IP --accountid B64 --pin 12345678 "
				                "[--out creds.txt] [--ps4|--ps5]\n");
				return 1;
		}
	}

	if(!host)      { fprintf(stderr, "Faltou --host.\n"); return 1; }
	if(!accountid) { fprintf(stderr, "Faltou --accountid.\n"); return 1; }
	if(!pin)       { fprintf(stderr, "Faltou --pin.\n"); return 1; }

	uint8_t account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t account_id_size = sizeof(account_id);
	if(chiaki_base64_decode(accountid, strlen(accountid), account_id, &account_id_size)
	       != CHIAKI_ERR_SUCCESS
	   || account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		fprintf(stderr, "accountid invalido: precisa decodificar para %d bytes em base64.\n",
			CHIAKI_PSN_ACCOUNT_ID_SIZE);
		return 1;
	}

	ChiakiRegistInfo info;
	memset(&info, 0, sizeof(info));
	info.target = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	info.host = host;
	info.broadcast = false;
	info.psn_online_id = NULL;
	memcpy(info.psn_account_id, account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE);
	info.pin = (uint32_t)strtoul(pin, NULL, 10);
	info.console_pin = 0;

	if(chiaki_mutex_init(&g_rmutex, false) != CHIAKI_ERR_SUCCESS)
		return 1;
	if(chiaki_cond_init(&g_rcond) != CHIAKI_ERR_SUCCESS)
		return 1;

	ChiakiRegist regist;
	ChiakiErrorCode err = chiaki_regist_start(&regist, log, &info, regist_cb, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_regist_start falhou: %s\n", chiaki_error_string(err));
		return 1;
	}

	fprintf(stderr, "[register] Pareando com %s (%s)... aguarde.\n", host, ps5 ? "PS5" : "PS4");

	chiaki_mutex_lock(&g_rmutex);
	while(!g_rdone)
		chiaki_cond_wait(&g_rcond, &g_rmutex);
	bool success = g_rsuccess;
	ChiakiRegisteredHost rhost = g_rhost;
	chiaki_mutex_unlock(&g_rmutex);

	chiaki_regist_fini(&regist);

	if(!success)
	{
		fprintf(stderr, "[register] Falhou. Confira IP, PIN (gere um novo no console) e account id.\n");
		return 1;
	}

	char registkey[CHIAKI_SESSION_AUTH_SIZE + 1];
	memcpy(registkey, rhost.rp_regist_key, CHIAKI_SESSION_AUTH_SIZE);
	registkey[CHIAKI_SESSION_AUTH_SIZE] = '\0';

	char morning_hex[sizeof(rhost.rp_key) * 2 + 1];
	for(size_t i = 0; i < sizeof(rhost.rp_key); i++)
		snprintf(morning_hex + i * 2, 3, "%02x", rhost.rp_key[i]);

	fprintf(stderr, "\n[register] Sucesso!\n\n");
	printf("host=%s\n", host);
	printf("console=%s\n", ps5 ? "ps5" : "ps4");
	printf("registkey=%s\n", registkey);
	printf("morning=%s\n", morning_hex);
	printf("accountid=%s\n", accountid);
	printf("\n# Comando pronto:\n");
	printf("digitador type --%s --host %s --registkey %s --morning %s --accountid %s\n",
		ps5 ? "ps5" : "ps4", host, registkey, morning_hex, accountid);

	if(out)
	{
		FILE *f = fopen(out, "w");
		if(f)
		{
			fprintf(f, "host=%s\nconsole=%s\nregistkey=%s\nmorning=%s\naccountid=%s\n",
				host, ps5 ? "ps5" : "ps4", registkey, morning_hex, accountid);
			fclose(f);
			fprintf(stderr, "[register] Credenciais salvas em %s\n", out);
			fprintf(stderr, "[register] Agora rode: digitador type --creds %s\n", out);
		}
		else
			fprintf(stderr, "[register] Nao consegui escrever em %s\n", out);
	}

	return 0;
}

// ------------------------------------------------------------------- main ----

static void usage(void)
{
	fprintf(stderr,
		"digitador - digitacao livre no PS5/PS4 pelo teclado do PC\n\n"
		"Uso:\n"
		"  digitador register --host IP --accountid B64 --pin 12345678 [--out creds.txt] [--ps4]\n"
		"  digitador type --creds creds.txt\n"
		"  digitador type --host IP --registkey K --morning HEX --accountid B64 [--ps4]\n");
}

int main(int argc, char *argv[])
{
	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, NULL);

	if(argc < 2)
	{
		usage();
		return 1;
	}
	if(strcmp(argv[1], "type") == 0)
		return cmd_type(&log, argc - 1, argv + 1);
	if(strcmp(argv[1], "register") == 0)
		return cmd_register(&log, argc - 1, argv + 1);

	usage();
	return 1;
}
