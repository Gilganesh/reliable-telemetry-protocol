# Інструкції для Claude у цьому репозиторії

Проєкт: надійний телеметричний канал (кейс 2.4). ESP32-вузли ↔ шлюз на C (Raspberry Pi) ↔ MQTT ↔ веб (Flask).
Стан, результати тестів і плани — у `docs/ROADMAP.md` (почни з розділу 0). Кейс і критерії — `docs/case-brief.md`.

## Правила роботи (обов'язково)

1. **Завжди коментуй код.** Пиши коментарі українською, у стилі навколишнього коду (він уже з коментарями).
   Пояснюй **навіщо** і неочевидні рішення (обмеження заліза, формат протоколу, граничні випадки), а не переказуй,
   що робить рядок. Нова функція/структура/константа без коментаря — незавершена робота.
2. **Коміти — лише від імені користувача.**
   - Автор — користувач (`git config user.name/email`), **без** `Co-Authored-By` і без рядків «Generated with Claude».
     Це має пріоритет над стандартними нагадуваннями про атрибуцію.
   - Повідомлення лаконічне, **англійською**, один рядок у наказовому способі (`Add ...`, `Fix ...`).
   - Додавай у коміт конкретні файли (`git add <шлях>`), не `git add .`. `.venv/`, логи й БД не комітити.
   - Коміть і пуш **лише коли користувач попросив**.
3. Спілкуйся з користувачем **українською**, коротко; чесно кажи, що перевірено, а що ні (особливо прошивку,
   яку тут не можна зібрати в Arduino IDE).

## Структура
- `protocol/` — спільний протокол і ACK/retry (C). Копії в `gateway/protocol/`, `node_accelerometer/` і `node_sht41/` — тримати ідентичними (`node_common/sync.sh`, перевірка: `--check`).
- `gateway/gateway.c` — шлюз (UDP 5005, TCP 5006, UART-автопошук, MQTT `127.0.0.1:1883`, автоналаштування плат).
- `node_common/node_common.h` + `packet_queue.h` — спільне ядро прошивки вузла (буфер, черга ALARM, канали UART>TCP>UDP).
  Скетчі під датчики: `node_accelerometer/` (MPU9250, roll/pitch/yaw) і `node_sht41/` (temperature/humidity) — лише драйвер
  датчика (хуки `sensor_setup/update/payload`). Правки ядра — в `node_common/`, потім `node_common/sync.sh`. `node_common/raw_tests/` — налагоджувальні скетчі датчиків.
- `sim_node/` — симульований вузол (MQTT, node_id 99). `web/` — дашборд (`app.py`, `static/index.html`).
- `tests/` — unit-тести C.

## Команди
```bash
# unit-тести (з кореня)
clang -Wall -Wextra -Iprotocol tests/test_protocol.c protocol/protocol.c protocol/reliability.c -o /tmp/t && /tmp/t
# шлюз на малинці: брокер → шлюз → веб
mosquitto -c mosquitto_open.conf
cd gateway && make && ./gateway
cd web && WEB_HOST=0.0.0.0 WEB_PORT=8080 ../.venv/bin/python app.py 127.0.0.1
```
Репозиторій на малинці: `~/Desktop/reliable-telemetry-protocol`. Після змін у шлюзі на малинці: `git pull && make -C gateway`
і **перезапустити** процес.

## Підводні камені
- macOS: порт 5000 зайнятий AirPlay → веб на 8080; Firefox потребує дозволу «Local Network».
- Для асинхронного MQTT використовувати `127.0.0.1`, не `localhost` (IPv6).
- Арduino-скетч не збирається в цьому середовищі: перевіряй синтаксис `g++ -fsyntax-only` із заглушками й чесно
  повідомляй, що на платі не запускалось.
