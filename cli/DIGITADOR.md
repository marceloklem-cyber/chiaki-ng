# Digitador — digitação livre no PS5/PS4 pelo teclado do PC

Adiciona dois subcomandos ao `chiaki-cli`:

- **`register`** — pareia o PC com o console (uma vez) e imprime as credenciais.
- **`type`** — abre uma sessão de Remote Play **sem vídeo, sem áudio e sem controle**,
  só para digitar nos campos de texto do console usando o teclado do PC. Aceita
  UTF-8 livre (acentos, ç, til, outro idioma, emoji).

A imagem do jogo continua na TV do console; este programa só cuida do texto.

## 1. Compilar (apenas o CLI, sem a GUI do Qt)

Linux / macOS / MSYS2 (Windows):

```sh
git clone --recursive https://github.com/streetpea/chiaki-ng.git
cd chiaki-ng
git apply digitador.patch          # aplica as mudanças do digitador
cmake -B build -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_TESTS=OFF
cmake --build build --target chiaki-cli
```

O binário sai em `build/cli/chiaki-cli`.

> Dependências da libchiaki (OpenSSL, protobuf, etc.) precisam estar instaladas.
> No Windows o caminho recomendado é MSYS2/MinGW ou o vcpkg que o projeto já usa.

## 2. Registrar o console (uma vez)

No console: **Ajustes → Sistema → Remote Play → Vincular Dispositivo** — anote o PIN de 8 dígitos.

Você também precisa do seu **PSN Account ID em base64**. Se você já usa o chiaki-ng,
ele está nas configurações da conta (campo "PSN Account ID"). 

```sh
./build/cli/chiaki-cli register --ps5 \
    --host 10.0.0.143 \
    --accountid SEU_ACCOUNT_ID_BASE64 \
    --pin 12345678 \
    --out creds.txt
```

Isso imprime `registkey`, `morning` e `accountid`, e salva tudo em `creds.txt`.

## 3. Digitar

```sh
./build/cli/chiaki-cli type --creds creds.txt
```

Conecte, abra um campo de texto no console, digite a linha no terminal do PC e
pressione **Enter** — o texto vai para o campo focado. **Ctrl+D** encerra.

Sem o arquivo de credenciais, dá para passar tudo na mão:

```sh
./build/cli/chiaki-cli type --ps5 --host 10.0.0.143 \
    --registkey XXXXXXXX --morning <32 hex> --accountid <base64>
```
