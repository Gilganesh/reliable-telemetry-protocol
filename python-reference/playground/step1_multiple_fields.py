"""
КРОК 1. Тепер не одне поле, а три — як маленька "структура заголовка".

У C це було б:

    struct SensorHeader {
        uint8_t  node_id;    // від якого вузла
        uint8_t  msg_type;   // 0 = heartbeat, 1 = telemetry (як enum)
        uint32_t sequence;   // порядковий номер повідомлення
    };

Головне правило (те саме, що і в C-структурах): ПОРЯДОК полів у форматі
має збігатися з порядком, у якому ти їх пакуєш і розпаковуєш. Переплутав
порядок на одній стороні — і дані "з'їхали", хоч код і не впаде з помилкою.

Запуск:
    python3 playground/step1_multiple_fields.py
"""
import struct

# struct SensorHeader { uint8_t node_id; uint8_t msg_type; uint32_t sequence; };
FORMAT = "!BBI"
#          ^^^
#          ||I = unsigned int, 4 байти  -> sequence
#          |B = unsigned char, 1 байт   -> msg_type
#          B = unsigned char, 1 байт    -> node_id

node_id = 5
msg_type = 1  # домовились: 1 = TELEMETRY (як enum-значення)
sequence = 42

raw_bytes = struct.pack(FORMAT, node_id, msg_type, sequence)

print("Запакували:")
print(f"  node_id  = {node_id}")
print(f"  msg_type = {msg_type}")
print(f"  sequence = {sequence}")
print()
print(f"Байти (hex): {raw_bytes.hex()}   довжина: {len(raw_bytes)} байт")
print()

node_id_b, msg_type_b, sequence_b = struct.unpack(FORMAT, raw_bytes)

print("Розпакували назад:")
print(f"  node_id  = {node_id_b}")
print(f"  msg_type = {msg_type_b}")
print(f"  sequence = {sequence_b}")
