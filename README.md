# ESP32 Modbus Config Backend (ESP-IDF)

Прошивка для **ESP32-DevKitV1 (ESP32-WROOM-32)**, реализующая серверную часть конфигуратора Modbus.

Проект построен на **ESP-IDF 5.5.3** и использует **FreeRTOS** для многозадачной обработки запросов.

## Назначение

Устройство поднимает Wi‑Fi сеть (SoftAP), принимает HTTP JSON‑RPC запросы и выполняет Modbus RTU обмен через UART.

- Wi‑Fi SSID: `MCBackend`
- HTTP endpoint: `http://<ip_платы>:8080/rpc`

## Архитектура (4 слоя)

### 1) Transport Layer
Отвечает за UART транспорт:
- открытие / закрытие / переключение UART;
- байтовый обмен (TX/RX);
- контроль статуса транспорта.

### 2) Protocol Layer
Отвечает за Modbus RTU кадры:
- построение ADU запросов;
- разбор ответов;
- CRC16;
- парсинг адресов регистров из decimal и hex-строки.

### 3) Application Layer
Бизнес-логика и orchestration:
- управление транспортом;
- постановка задач чтения/записи в очередь;
- выполнение Modbus операций в рабочей FreeRTOS задаче.

### 4) API Layer
HTTP JSON‑RPC сервер:
- прием POST запросов;
- валидация параметров;
- преобразование DTO ↔ вызовы Application Layer;
- JSON‑RPC ответы/ошибки.

## Поддерживаемые JSON‑RPC методы

- `ping`
- `transport.status`
- `transport.serial_ports`
- `transport.open`
- `transport.switch`
- `transport.close`
- `modbus.read`
- `modbus.read_group`
- `modbus.write`
- `modbus.write_group`

## Поддержка адресов регистров

Поле `address` поддерживает:
- decimal: `100`
- hex-string: `0x0064`, `0xF020`

## Формат ответа на чтение

Для операций чтения возвращаются поля:
- `slave_id`
- `address`
- `count`
- `function`
- `values`
- `ok`

## Быстрый старт

### 1. Выбор target
```bash
idf.py set-target esp32
```

### 2. Сборка
```bash
idf.py build
```

### 3. Прошивка и монитор
```bash
idf.py -p <PORT> flash monitor
```

После запуска:
1. Подключитесь к Wi‑Fi сети `MCBackend`.
2. Отправляйте JSON‑RPC POST запросы на `http://<ip_платы>:8080/rpc`.

## Пример запроса

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "ping",
  "params": {}
}
```

## Пример ответа
=======

Поле `address` поддерживает:
- decimal: `100`
- hex-string: `0x0064`, `0xF020`

## Формат ответа на чтение

Для операций чтения возвращаются поля:
- `slave_id`
- `address`
- `count`
- `function`
- `values`
- `ok`

## Быстрый старт

### 1. Выбор target
```bash
idf.py set-target esp32
```

### 2. Сборка
```bash
idf.py build
```

### 3. Прошивка и монитор
```bash
idf.py -p <PORT> flash monitor
```

После запуска:
1. Подключитесь к Wi‑Fi сети `MCBackend`.
2. Отправляйте JSON‑RPC POST запросы на `http://<ip_платы>:8080/rpc`.

## Пример запроса

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "status": "ok"
  }
}
```

## Требования

- ESP-IDF `v5.5.3`
- Плата ESP32 DevKit (ESP32-WROOM-32)
- USB-UART подключение для прошивки/логов
- Modbus RTU устройство на UART линии

## Примечания

- Проект ориентирован на промышленный сценарий: слоистая архитектура, многозадачность, расширяемость.
- Рекомендуется фиксировать параметры UART и таймауты под конкретную нагрузку и линию связи.
