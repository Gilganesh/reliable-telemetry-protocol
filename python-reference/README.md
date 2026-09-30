# python-reference/ — v1 прототип (Python), НЕ deliverable

Це перша версія протоколу і прототипу, написана і повністю протестована
28-29 вересня на Python (UDP-транспорт, власний бінарний формат пакета).

**Команда вирішила перейти на C + ESP32 + MQTT (див.
`../docs/team-blocks/00-overview-shared-contract.md`).** Код тут
лишається як довідник — уся логіка (CRC-перевірка, gap/duplicate
детекція, dashboard) вже перевірена і працює, і корисна як приклад "як
це має поводитись", коли переносиш ту саму ідею на C.

Не редагуй і не будуй нове на цьому коді — це не той код, що йде на
демонстрацію. Актуальний код — у `../protocol/`, `../node_esp32/`,
`../gateway/`, `../sim_node/`.

Запуск (якщо цікаво подивитись, як воно працює):
```
python3 -m unittest discover -s tests -v
python3 -m gateway.gateway --port 9999
python3 -m node.sensor_node --node-id 1 --gateway-port 9999
```
