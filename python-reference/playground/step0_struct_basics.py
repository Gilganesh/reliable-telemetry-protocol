"""
КРОК 0. Найпростіший приклад: як одне число з датчика перетворюється на байти.

Це той самий принцип, що і структура в C:

    struct SensorPacket {
        uint8_t  node_id;      // 1 байт
        float    temperature;  // 4 байти
    };

Тільки замість `struct {...}` в Python описуємо ту саму "форму" рядком-рецептом
для модуля struct. Нічого більше в цьому файлі немає — жодних класів,
жодного CRC, жодної мережі. Лише самі байти.

Запуск:
    python3 playground/step0_struct_basics.py
"""
import struct

# ==== 1. Опис "структури" ====
#
# У C ти написав би:
#   struct SensorPacket {
#       uint8_t  node_id;
#       float    temperature;
#   };
#
# У Python той самий "шаблон" описуємо рядком:
FORMAT = "!Bf"
#          ^^
#          | | f = float, 4 байти          -> temperature
#          B = unsigned char, 1 байт        -> node_id
#         ! = "мережевий порядок байтів" (big-endian) -- поки не важливо, деталь нижче

# ==== 2. Дані, які реально прийшли з "датчика" ====
node_id = 5
temperature = 21.4

# ==== 3. Пакуємо (те, що в C сталося б само, коли ти кидаєш struct у UART) ====
raw_bytes = struct.pack(FORMAT, node_id, temperature)

print("Що ми запакували:")
print(f"  node_id     = {node_id}")
print(f"  temperature = {temperature}")
print()
print("Що вийшло (сирі байти, саме це полетіло б у дріт/мережу):")
print(f"  довжина: {len(raw_bytes)} байт")
print(f"  hex:     {raw_bytes.hex()}")
print()

# ==== 4. Розпаковуємо назад (те, що робить приймач на іншому кінці) ====
node_id_back, temperature_back = struct.unpack(FORMAT, raw_bytes)

print("Що прочитав приймач після розпакування:")
print(f"  node_id     = {node_id_back}")
print(f"  temperature = {temperature_back}")
