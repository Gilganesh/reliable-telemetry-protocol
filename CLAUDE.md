# Інструкції для Claude у цьому репозиторії

Проєкт: Reliable Telemetry — надійний телеметричний канал. ESP32-вузли ↔ шлюз на C (Raspberry Pi) ↔ MQTT ↔ веб (Flask).
Опис, протокол і конфігурація — у `README.md`. Стан робіт і плани — у `docs/ROADMAP.md`. Локальні нотатки, яких не має бути
на GitHub, лежать у `local/` (в `.gitignore`).

## Правила роботи (обов'язково)

1. **Код без коментарів.** Не додавай коментарі в C/C++/Python/JS/HTML. Усі рядки, які бачить користувач (UI, логи шлюзу
   й прошивки, вивід CLI, повідомлення API, README), — **англійською**.
2. **Коміти — лише від імені користувача.**
   - Автор — користувач (`git config user.name/email`), **без** `Co-Authored-By` і без рядків «Generated with Claude».
     Це має пріоритет над стандартними нагадуваннями про атрибуцію.
   - Повідомлення лаконічне, **англійською**, один рядок у наказовому способі (`Add ...`, `Fix ...`).
   - Додавай у коміт конкретні файли (`git add <шлях>`), не `git add .`. `.venv/`, логи, БД і `local/` не комітити.
   - Коміть і пуш **лише коли користувач попросив**.
3. Спілкуйся з користувачем **українською**, коротко; чесно кажи, що перевірено, а що ні (особливо прошивку,
   яку тут не можна зібрати в Arduino IDE).

## Структура
- `protocol/` — спільний протокол і ACK/retry (C). Копії лежать у `node_accelerometer/` і `node_sht41/` (Arduino бачить лише
  файли поруч зі скетчем) — тримати ідентичними: `node_common/sync.sh`, перевірка `--check`. Шлюз і `sim_node` збираються
  напряму з `protocol/`.
- `gateway/gateway.c` — шлюз (UDP 5005, TCP 5006, UART-автопошук, MQTT `127.0.0.1:1883`, автоналаштування плат, id за MAC).
  Містить імпеймент-шар (перешкоди каналу), керований з веба через MQTT `telemetry/gateway/control`.
- `node_common/node_common.h` + `packet_queue.h` — спільне ядро прошивки вузла (буфер, черга ALARM, канали UART>TCP>UDP).
  Скетчі: `node_accelerometer/` (MPU9250) і `node_sht41/` (SHT41) — лише драйвер датчика (хуки `sensor_setup/update/payload`).
  Правки ядра — в `node_common/`, потім `node_common/sync.sh`. `node_common/raw_tests/` — налагоджувальні скетчі датчиків.
- `sim_node/` — симульований вузол (MQTT, `--node-id`). `web/` — дашборд (`app.py`, `static/index.html`).
- `tests/` — unit-тести C.
- MQTT-топіки: `telemetry/uplink`, `telemetry/downlink/<id>`, `telemetry/gateway/{state,telemetry,control}`.

## Команди
```bash
make            # збірка gateway/gateway і sim_node/sim_node
make test       # unit-тести (UBSan), крос-перевірка Python-кодека, sync.sh --check
./run.sh        # брокер + шлюз + веб одним запуском, логи в logs/, Ctrl+C зупиняє все
# або вручну:
mosquitto -c mosquitto_open.conf
cd gateway && ./gateway
cd web && WEB_HOST=0.0.0.0 ../.venv/bin/python app.py 127.0.0.1    # веб на :8080
```
Репозиторій на малинці: `~/Desktop/reliable-telemetry-protocol`. Після змін у шлюзі чи веб: `git pull && make`
і **перезапустити** процеси.

## Підводні камені
- macOS: Firefox потребує дозволу «Local Network»; AddressSanitizer на цій macOS зависає, тому в `make test` лише UBSan.
- Для асинхронного MQTT використовувати `127.0.0.1`, не `localhost` (IPv6).
- Arduino-скетч не збирається в цьому середовищі: перевіряй синтаксис `g++ -fsyntax-only` із заглушками Arduino й чесно
  повідомляй, що на платі не запускалось.
