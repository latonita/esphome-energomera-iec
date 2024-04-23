# ESPHome компонент для подключения счетчиков электроэнергии Энергомера CE102M CE301 CE303 CE308 по RS-485 (ГОСТ IEC 61107-2011)

* [Назначение](#назначение)
* [Отказ от ответственности](#отказ-от-ответственности)
* [Функции](#функции)
* [Пример отображения в home-assistant](#пример-отображения-в-home-assistant)
* [Подключение](#подключение)
* [Настройка основного компонента](#настройка-основного-компонента)
* [Настройка сенсоров для опроса счетчика](#настройка-сенсоров-для-опроса-счетчика)
* [Примеры готовых конфигураций](#примеры-готовых-конфигураций)
* [Проблемы, особенности, рекомендации](#проблемы-особенности-рекомендации)

## Назначение
Компонент для считывания данных с электросчетчиков, поддерживающих протокол МЭК/IEC 61107, таких как Энергомера СЕ102М, СЕ301, СЕ303. 
Потенциально может работать и сдругими счетчиками поддерживающими данный ГОСТ.

## Отказ от ответственности
Пользуясь данным ПО пользователь полностью берет на себя всю ответственность за любые последствия.
 
## Функции
- подключение как безадресное (широковещательный запрос), так и по адресу (9 последних цифр заводского номера),
- индивидуальна настройка сенсоров под конкретные запросы,
- работа с массивом возвращаемых значений,
- два вида сенсоров: числовой и текстовый,
- работа только на скорости 9600 бод.

## Пример отображения в home-assistant
![Пример отображения в home-asistant](/images/ha-303.png) 

## Подключение
Устройства:
- микроконтроллер Esp (работа провена на модулях на базе esp32, esp32s, esp32s2, esp8266)
  - если испольуется UART0, то в модуле логгера нужно отключать вывод в порт (baud_rate:0)
- модуль трансивера RS485,
  - должен быть расчитан на 3.3 вольта (связка Esp + модуль на max485 расчитаный на 5 вольт может работать некорректно)
  - запитываем либо от esp модуля, либо отдельно, земля общая
- счетчик электрической энергии (работа проверена со счетчиками Энергомера СЕ102М, СЕ301 версии CE301v11.8s4, СЕ303 версии CE303v11.8s4)
  - A+ и B- соединяем с модулем
    - желательно подключение витой парой
    - земли счетчика и модуля 485 *не соединяем*, а если кабель с экраном/оплеткой - можно соеденить с землей только со стороны модуля для уменьшения наводок
    - если расстояние до счетчика большое, то может понадобиться терминирующий резистор 120 Ом между A и B
  - CE301/CE303 в корпусах R32/R33 - необходимо отдельно подавать питание 9-12 вольт на клеммы V+, V-.
  - пользуемся документацией на счетчик для уточнения схем подключений
  
### Рекомендуемый вариант подключения с RS-485 модулем с автоматическим выбором направления передачи 
На данный момент в продаже много модулей RS-485 с расширеным набором функций - с логикой автоматического выбора направления передачи, защитными диодами и предохранителями. 
Эти модули рекомендуются к использованию, т.к. более надежны. 
Например, модуль XY-017

![Модуль XY-017](images/xy017.jpg)

```
+-------+                    +-------------+              +----------------+
|  MCU  | RX <----------< RX | RS485<->TTL | A <------> A | Электросчетчик |
| ESPxx | TX >----------> TX |   module    |              |                |
|       | GND ---------- GND |             | B <------> B |                |
+-------+                    +-------------+              +----------------+
```
Иногда RX/TX на модуле перепутаны - ничего страшного, просто меняем.

### Вариант подключения с классическим RS-485 модулем с 4 входами
Классический модуль с RO/DI/DE/RE входами + общая земля.
``RO`` - прием, 
``DI`` - передача, 
``DE + R̅E̅``  - контроль линии для передачи данных (может отсутствовать на модуле, тогда строчка flow_control_pin не нужна)
```
+-------+                    +-------------+              +----------------+
|  MCU  | RX <----------< RO | RS485<->TTL | A <------> A | Электросчетчик |
|       |             ,-> R̅E̅ |             |              |                |
| ESPxx | FLOW >-----+       |   module    | B <------> B |                |
|       |             `-> DE |    3.3v!    |              |                |
|       | TX >----------> DI |             |              |                |
|       | GND ---------- GND |             |              |                |
+-------+                    +-------------+              +----------------+
```

## Настройка основного компонента
Подлючаем внешний компонент из репозитория
```
external_components:
  - source: github://latonita/esphome-energomera-iec
    refresh: 30s
    components: [energomera_iec]
```
Конфигурируем UART 9600 7E1:
```
uart:
  rx_pin: GPIO16
  tx_pin: GPIO17
  baud_rate: 9600
  data_bits: 7
  parity: EVEN
  stop_bits: 1
```

Основной модуль
```
energomera_iec:
  id: ce102m
  update_interval: 30s
#  address: 123456789
#  receive_timeout: 500ms         # время ожидания ответа от счетчика
#  delay_between_requests: 100ms  # задержка между запросами к счетчику
#  flow_control_pin: GPIO32
```
- `address` - по-умолчанию пустой, если счетчик один - то адрес не требуется. Если несколько счетчиков - то там указываем его адрес - это последние 9 цифр его заводского номера.
- `receive_timeout` - по-умолчанию 500мс, если ответы длинные - то можем не успеть дождаться ответа - увеличиваем.
- `delay_between_requests` - по-умолчанию 100мс, иногда счетчик может тупить после больших запросов и не успевает принять новый - увеличиваем. **важно** - больше 1.5с не рекомендую, в счетчиках есть таймаут от 1.5с до 3с - если их не дергают, они считают, что общение закончено и закрывают сессию.
- `flow_control_pin` - указываем, если 485 модуль требует сигнал направления передачи RE/DE 

## Настройка сенсоров для опроса счетчика
Реализованы два типа сенсоров:
- `sensor` - числовые данные, float
- `text_sensor` - текстовые данные в формате "как пришли от счетчика"
```
sensor/text_sensor:
  - platform: energomera_iec
    name: Название сенсора
    request: ЗАПРОС()
    index: индекс ответа
    ... остальные стандартные параметры для сенсора ...
```

Названия функций для запроса берем из документации на счетчик. Усли запрос возвращает несколько значений, то, по-умолчанию, берется первое, но можно выбрать указав номер ответа (индекс, начинается с 1).
Примеры запросов и ответов от счетчика:
| Счетчик | Запрос | Ответ счетчика | Индекс | Результат |
|--|--|--|--|--|
| CE102M    | `VOLTA()` | `VOLTA(228.93)`| не указан | 228.93 |
| CE301/303 | `VOLTA()` | `VOLTA(228.93)VOLTA(230.02)VOLTA(235.12)` | не указан | 228.93 |
| CE301/303 | `VOLTA()` | `VOLTA(228.93)VOLTA(230.02)VOLTA(235.12)` | 1 | 228.93 |
| CE301/303 | `VOLTA()` | `VOLTA(228.93)VOLTA(230.02)VOLTA(235.12)` | 2 | 230.02 |
| *         | `ET0PE()` | `ET0PE(34261.8262567)(25179.1846554)(9082.6416013)(0.0)(0.0)(0.0)` | 2 | 25179.1846554 |

Запросы берем из руководств на счетчики. Например, [Руководство по эксплуатации CE102M](http://sp.energomera.ru/documentations/product/ce102m_re_full.pdf) , или 
[Руководство пользователя CE301 и CE303](http://sp.energomera.ru/documentations/product/ce301_303_rp.pdf).

### Пример 1. Потребление кВт*ч
```
sensor:
  - platform: energomera_iec
    request: ET0PE()
    index: 2
    name: Энергия Тариф 1
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    request: ET0PE()
    index: 3
    name: Энергия Тариф 2
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing
```
### Пример 2. Дата
Дату счетчик возвращает в формате `нн.дд.мм.гг`, где - день недели 00 - воскресенье, 01 понедельник. 
Превратить это в нормальную дату можно, например,так:
```
text_sensor:
  - platform: energomera_iec
    name: Date
    request: DATE_()
    filters:
      - lambda: |-
          std::string str{x};
          str.erase(0,3);
          str.insert(6,"20");
          return str;
```

## Примеры готовых конфигураций

<details><summary>Для однофазного счетчика CE102M</summary>

* [Скачать конфиг ce102m.yaml](ce102m.yaml)

```
esphome:
  name: energomera-ce102m

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

external_components:
  - source: github://latonita/esphome-energomera-iec
    refresh: 30s
    components: [energomera_iec]

uart:
  rx_pin: GPIO16
  tx_pin: GPIO17
  baud_rate: 9600
  data_bits: 7
  parity: EVEN
  stop_bits: 1

energomera_iec:
  id: ce102m
  update_interval: 30s
#  receive_timeout: 500ms
#  delay_between_requests: 150ms
#  flow_control_pin: GPIO32

sensor:
  - platform: energomera_iec
    request: ET0PE()
    index: 1
    name: Электроэнергия
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    request: ET0PE()
    index: 2
    name: Электроэнергия T1
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    request: ET0PE()
    index: 3
    name: Электроэнергия T2
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    name: Ток
    request: CURRE()
    unit_of_measurement: A
    accuracy_decimals: 2
    device_class: current
    state_class: measurement

  - platform: energomera_iec
    name: Напряжение
    request: VOLTA()
    unit_of_measurement: V
    accuracy_decimals: 1
    device_class: voltage
    state_class: measurement

  - platform: energomera_iec
    name: Частота
    request: FREQU()
    unit_of_measurement: Hz
    accuracy_decimals: 2
    device_class: frequency
    state_class: measurement

  - platform: energomera_iec
    name: Коэффициент мощности
    request: COS_f()
    unit_of_measurement: "%"
    accuracy_decimals: 2
    device_class: power_factor
    state_class: measurement

  - platform: energomera_iec
    name: Активная мощность
    request: POWEP()
    unit_of_measurement: kW
    accuracy_decimals: 3
    device_class: power
    state_class: measurement

text_sensor:
  - platform: energomera_iec
    name: Заводской номер
    request: SNUMB()
    entity_category: diagnostic

  - platform: energomera_iec
    name: Время
    request: TIME_()
    entity_category: diagnostic

  - platform: energomera_iec
    name: Дата
    request: DATE_()
    entity_category: diagnostic
    filters:
      - lambda: |-
          std::string str{x};
          str.erase(0,3);
          str.insert(6,"20");
          return str;

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  reboot_timeout: 5min
  power_save_mode: NONE

api:
  password: !secret api_password

ota:
  password: !secret ota_password

```

</details>

<details><summary>Для трехфазных счетчиков CE301, CE303</summary>

* [Скачать конфиг ce303.yaml](ce303.yaml)

```
esphome:
  name: energomera-ce303

esp8266:
  board: nodemcuv2

logger:
  level: DEBUG

external_components:
  - source: github://latonita/esphome-energomera-iec
    refresh: 10s
    components: [energomera_iec]

uart:
  rx_pin: D5
  tx_pin: D6
  baud_rate: 9600
  data_bits: 7
  parity: EVEN
  stop_bits: 1
  rx_buffer_size: 512

energomera_iec:
  id: ce303
  address: 123456789
  update_interval: 10s
#  delay_between_requests: 50ms
#  receive_timeout: 500ms

sensor:
  - platform: energomera_iec
    request: ET0PE()
    index: 1
    name: Электроэнергия
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    request: ET0PE()
    index: 2
    name: Электроэнергия T1
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    request: ET0PE()
    index: 3
    name: Электроэнергия T2
    unit_of_measurement: kWh
    accuracy_decimals: 3
    device_class: energy
    state_class: total_increasing

  - platform: energomera_iec
    name: Ток фаза A
    request: CURRE()
    index: 1
    unit_of_measurement: A
    accuracy_decimals: 3
    device_class: current
    state_class: measurement

  - platform: energomera_iec
    name: Ток фаза B
    request: CURRE()
    index: 2
    unit_of_measurement: A
    accuracy_decimals: 3
    device_class: current
    state_class: measurement

  - platform: energomera_iec
    name: Ток фаза C
    request: CURRE()
    index: 3
    unit_of_measurement: A
    accuracy_decimals: 3
    device_class: current
    state_class: measurement

  - platform: energomera_iec
    name: Напряжение фаза A
    request: VOLTA()
    index: 1
    unit_of_measurement: V
    accuracy_decimals: 3
    device_class: voltage
    state_class: measurement

  - platform: energomera_iec
    name: Напряжение фаза B
    request: VOLTA()
    index: 2
    unit_of_measurement: V
    accuracy_decimals: 3
    device_class: voltage
    state_class: measurement

  - platform: energomera_iec
    name: Напряжение фаза C
    request: VOLTA()
    index: 3
    unit_of_measurement: V
    accuracy_decimals: 3
    device_class: voltage
    state_class: measurement

  - platform: energomera_iec
    name: Активная мощность
    request: POWEP()
    index: 1
    unit_of_measurement: kW
    accuracy_decimals: 3
    device_class: power
    state_class: measurement

  - platform: energomera_iec
    name: Активная мощность фаза A
    request: POWPP()
    unit_of_measurement: kW
    accuracy_decimals: 3
    index: 1
    device_class: power
    state_class: measurement

  - platform: energomera_iec
    name: Активная мощность фаза B
    request: POWPP()
    unit_of_measurement: kW
    accuracy_decimals: 3
    index: 2
    device_class: power
    state_class: measurement

  - platform: energomera_iec
    name: Активная мощность фаза C
    request: POWPP()
    unit_of_measurement: kW
    accuracy_decimals: 3
    index: 3
    device_class: power
    state_class: measurement

text_sensor:
  - platform: energomera_iec
    name: Заводской номер
    request: SNUMB()
    entity_category: diagnostic

  - platform: energomera_iec
    name: Время
    request: TIME_()
    entity_category: diagnostic

  - platform: energomera_iec
    name: Дата
    request: DATE_()
    entity_category: diagnostic
    filters:
      - lambda: |-
          std::string str{x};
          str.erase(0,3);
          str.insert(6,"20");
          return str;

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  password: !secret api_password

ota:
  password: !secret ota_password

```

</details>

## Проблемы, особенности, рекомендации
- "да должно всё работать" :)
- при слабом сигнале wifi, esp может терять точку доступа и часто заново подключаться. А если esp одноядерная (например, esp8266 или esp32s2), то это может влиять на сбор данных со счетчика и, в целом, может приводить к неожиданным перезагрузкам
- по идее, hardware uart всегда надежнее, но, в целом, software uart и на esp8266 работает без сбоев, если не делать запросы, которые возвращают кучу данных
- забавно, что у кого-то бывает работает только software uart, у кого-то - только hardware uart, у многих - и так и так
- модуль 485 желательно брать с защитными диодами - наблюдали как умершую esp8266, так и модуль, у которого работала только отправка, а прием не работал
- модули 485 с маркировкой пинов rx/tx - иногда rx/tx наоборот :)
- внимательно смотрите на номера пинов на esp - ориентируйтесь на распиновку модулей (pinout diagram). Часто производители нумеруют пины на плате совсем не так, как они пронумерованы на чипе esp (например, nodemcu). Указывайте номера, как на чипе (GPIOxx).
  
