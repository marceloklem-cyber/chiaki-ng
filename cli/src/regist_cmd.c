// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// "register" - pareia o PC com o console e gera as credenciais que o
// subcomando "type" (digitador) precisa: registkey, morning (rp_key) e
// account id. Faça isso UMA vez por console.
//
// No console: Ajustes -> Sistema -> Remote Play -> Vincular Dispositivo,
// anote o PIN de 8 digitos e use em --pin.

#include <chiaki-cli.h>

#include <chiaki/regist.h>
#include <chiaki/base64.h>
#include <chiaki/thread.h>

#include <argp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

static char doc[] =
	"Registra (pareia) um PS5/PS4 e imprime as credenciais do digitador.\n"
	"No console: Ajustes -> Sistema -> Remote Play -> Vincular Dispositivo "
	"para obter o PIN de 8 digitos.";

#define ARG_KEY_HOST      'h'
#define ARG_KEY_ACCOUNTID 'a'
#define ARG_KEY_PIN       'p'
#define ARG_KEY_OUT       'o'
#define ARG_KEY_PS4       '4'
#define ARG_KEY_PS5       '5'

static struct argp_option options[] = {
	{ "host",      ARG_KEY_HOST,      "Host",      0, "Console IP address", 0 },
	{ "accountid", ARG_KEY_ACCOUNTID, "AccountID", 0, "PSN Account ID, base64 (8 bytes encoded)", 0 },
	{ "pin",       ARG_KEY_PIN,       "Pin",       0, "8-digit registration PIN from the console", 0 },
	{ "out",       ARG_KEY_OUT,       "File",      0, "Also save credentials to this file", 0 },
	{ "ps4",       ARG_KEY_PS4,       NULL,        0, "PlayStation 4", 0 },
	{ "ps5",       ARG_KEY_PS5,       NULL,        0, "PlayStation 5 (default)", 0 },
	{ 0 }
};

typedef struct arguments
{
	const char *host;
	const char *accountid;
	const char *pin;
	const char *out;
	bool ps5;
} Arguments;

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	Arguments *a = state->input;
	switch(key)
	{
		case ARG_KEY_HOST:      a->host = arg; break;
		case ARG_KEY_ACCOUNTID: a->accountid = arg; break;
		case ARG_KEY_PIN:       a->pin = arg; break;
		case ARG_KEY_OUT:       a->out = arg; break;
		case ARG_KEY_PS4:       a->ps5 = false; break;
		case ARG_KEY_PS5:       a->ps5 = true; break;
		case ARGP_KEY_ARG:      argp_usage(state); break;
		default:                return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

// synchronization between the regist thread (callback) and this command.
static ChiakiMutex g_mutex;
static ChiakiCond g_cond;
static bool g_done = false;
static bool g_success = false;
static ChiakiRegisteredHost g_host;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	(void)user;
	chiaki_mutex_lock(&g_mutex);
	if(event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
	{
		g_success = true;
		g_host = *event->registered_host;
	}
	g_done = true;
	chiaki_cond_signal(&g_cond);
	chiaki_mutex_unlock(&g_mutex);
}

CHIAKI_EXPORT int chiaki_cli_cmd_register(ChiakiLog *log, int argc, char *argv[])
{
	Arguments arguments = { 0 };
	arguments.ps5 = true;
	if(argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &arguments) != 0)
		return 1;

	if(!arguments.host)      { fprintf(stderr, "Faltou --host.\n"); return 1; }
	if(!arguments.accountid) { fprintf(stderr, "Faltou --accountid.\n"); return 1; }
	if(!arguments.pin)       { fprintf(stderr, "Faltou --pin.\n"); return 1; }

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

	ChiakiRegistInfo info;
	memset(&info, 0, sizeof(info));
	info.target = arguments.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	info.host = arguments.host;
	info.broadcast = false;
	info.psn_online_id = NULL; // modern consoles use the account id below
	memcpy(info.psn_account_id, account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE);
	info.pin = (uint32_t)strtoul(arguments.pin, NULL, 10);
	info.console_pin = 0;

	if(chiaki_mutex_init(&g_mutex, false) != CHIAKI_ERR_SUCCESS)
		return 1;
	if(chiaki_cond_init(&g_cond) != CHIAKI_ERR_SUCCESS)
		return 1;

	ChiakiRegist regist;
	ChiakiErrorCode err = chiaki_regist_start(&regist, log, &info, regist_cb, NULL);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "chiaki_regist_start falhou: %s\n", chiaki_error_string(err));
		return 1;
	}

	fprintf(stderr, "[register] Pareando com %s (%s)... aguarde.\n",
		arguments.host, arguments.ps5 ? "PS5" : "PS4");

	chiaki_mutex_lock(&g_mutex);
	while(!g_done)
		chiaki_cond_wait(&g_cond, &g_mutex);
	bool success = g_success;
	ChiakiRegisteredHost host = g_host;
	chiaki_mutex_unlock(&g_mutex);

	chiaki_regist_fini(&regist);

	if(!success)
	{
		fprintf(stderr, "[register] Falhou. Verifique IP, PIN (gere um novo no console) e account id.\n");
		return 1;
	}

	// rp_regist_key is a zero-padded ASCII key.
	char registkey[CHIAKI_SESSION_AUTH_SIZE + 1];
	memcpy(registkey, host.rp_regist_key, CHIAKI_SESSION_AUTH_SIZE);
	registkey[CHIAKI_SESSION_AUTH_SIZE] = '\0';

	// rp_key (morning) printed as hex.
	char morning_hex[sizeof(host.rp_key) * 2 + 1];
	for(size_t i = 0; i < sizeof(host.rp_key); i++)
		snprintf(morning_hex + i * 2, 3, "%02x", host.rp_key[i]);

	fprintf(stderr, "\n[register] Sucesso!\n\n");
	printf("# Credenciais do digitador (guarde com seguranca)\n");
	printf("host=%s\n", arguments.host);
	printf("console=%s\n", arguments.ps5 ? "ps5" : "ps4");
	printf("registkey=%s\n", registkey);
	printf("morning=%s\n", morning_hex);
	printf("accountid=%s\n", arguments.accountid);
	printf("\n# Comando pronto:\n");
	printf("chiaki-cli type --%s --host %s --registkey %s --morning %s --accountid %s\n",
		arguments.ps5 ? "ps5" : "ps4", arguments.host, registkey, morning_hex, arguments.accountid);

	if(arguments.out)
	{
		FILE *f = fopen(arguments.out, "w");
		if(f)
		{
			fprintf(f, "host=%s\nconsole=%s\nregistkey=%s\nmorning=%s\naccountid=%s\n",
				arguments.host, arguments.ps5 ? "ps5" : "ps4", registkey, morning_hex, arguments.accountid);
			fclose(f);
			fprintf(stderr, "[register] Credenciais salvas em %s\n", arguments.out);
			fprintf(stderr, "[register] Agora rode: chiaki-cli type --creds %s\n", arguments.out);
		}
		else
			fprintf(stderr, "[register] Nao consegui escrever em %s\n", arguments.out);
	}

	return 0;
}
