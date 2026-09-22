# ESP32-CYD-Bike-Computer
Open-source ESP32-based bicycle computer with a graphical display, speed tracking, trip statistics, environmental sensors, and customizable UI

![ESP32-CYD-Bike-Computer](Images/light-theme.jpg)
![ESP32-CYD-Bike-Computer](Images/dark-theme.jpg)
![ESP32-CYD-Bike-Computer](Images/settings.jpg)

## Features

- Real-time speed measurement
- Average and maximum speed
- Trip distance and total odometer
- Temperature, atmospheric pressure and humidity monitoring
- Trip timer

- # About the Project (для русского языка листайте ниже)

The entire codebase was written by AI. It went through many iterations before I achieved the desired result, so the code may not be fully optimized.
This bike computer is installed on my personal KTM bicycle. That's also where the enclosure design comes from — it is inspired by the instrument cluster of the KTM Duke 790–890 motorcycles.
The mounting system should also fit bicycles from other brands with the following dimensions:

* Fork tube diameter: 35 mm
* Handlebar diameter at the mounting point: 32 mm
For other dimensions, you may need to make minor modifications to the 3D models of the mounting brackets.

---

## Assembly
Assembly is straightforward, but it requires basic soldering skills and some understanding of electrical circuits.

**The most important rule: always connect positive (+) to positive (+) and negative (−) to negative (−).**

If you don't understand the difference between positive and negative polarity, ask someone with electrical experience for help.

An assembly diagram is available in the `assembly photos` folder.

**Disclaimer:** I am not responsible for any damage caused by incorrect wiring or reversed polarity, including damage to microchips or other electronic components.

---

## Basic Operation

Using the bike computer is simple — turn it on and ride!

To turn it off, press and hold the button for 10 seconds.

### Timer

* Tap the **Timer** area to switch between active timers.
* Long-press the Timer area to reset the currently selected timer.

### Trip

* Tap the **Trip** area to switch between Trip 1 and Trip 2.
* Long-press the Trip area to reset the selected trip. This also resets the maximum and average speed recorded for that trip.

The total odometer runs continuously and saves its value to non-volatile memory every 100 meters.

### Battery Indicator

Tap the battery level indicator to switch between percentage (%) and voltage (V).

---

## Settings

Tap the wrench icon to open the Settings page.

### 1. Display Theme

Choose between three display modes:

* Light theme
* Dark theme
* Automatic switching based on the ambient light sensor

### 2. Wheel Circumference

The default wheel circumference is **2275 mm**, with an adjustment step of **5 mm**.

Alternatively, you can set the exact value directly in the source code.

Find the following line:

```cpp
float wheelCircumferenceMm = 2275;
```

Replace `2275` with your measured wheel circumference in millimeters.

### 3. Number of Wheel Magnets

Set the number of magnets installed on the wheel.

Using more magnets provides smoother speed measurements.

**Important:** If you use more than one magnet, they must be evenly spaced around the wheel, with an equal number of spokes between them.

### 4. Set Hours

Adjust the current hour.

### 5. Set Minutes

Adjust the current minute.

**Important:** The date is set automatically when the firmware is compiled. Therefore, the firmware must be compiled and uploaded on the same day to ensure the correct date.

### 6. Measurement Units

Choose between two measurement systems:

| Metric | Imperial |
| ------ | -------- |
| km     | mi       |
| km/h   | mph      |
| °C     | °F       |
| mmHg   | inHg     |

---

## How to Accurately Measure Wheel Circumference

Accurate wheel circumference measurement is important for correct speed and distance calculations.

Follow these steps:

1. Place your bicycle on a flat surface and position the wheel so that the tire valve is at the bottom, closest to the ground.
2. Make a mark on the ground directly below the valve.
3. Roll the bicycle forward until the wheel completes exactly one full revolution and the valve returns to the bottom.
4. Make a second mark on the ground.
5. Use a measuring tape to measure the distance between the two marks.

The measured distance is your wheel circumference in millimeters.

Enter this value in the bike computer settings or directly in the source code.


  Код полностью написан ИИ. Он прошел много итерации пока я не получил нужный результат. Возможно не является оптимальным. 
  Установлен на личный велосипед марки KTM. Отсюда и выбранный корпус под плату - внешне стилизован под приборную панель от мотоциклов KTM Duke 790-890. Так же подойдет и на другие марки с диаметром пера вилки 35мм, диаметр руля в месте крепления — 32мм. Для других размеров возможно вам придется немного доработать 3d модели крепежа.
  Сборка простая, но требует небольшие навыки пайки и понимания в электрике. Главное правило всегда соединять плюс к плюсу, минус к минусу. Если вы не понимаете отличие плюса от минуса, попросите помощи у знающего человека. В папке assembly photos лежит схема сборки. Я не несу ответственность если вы перепутаете полярность и спалите микросхему или другой элемент.
  Использование простое — включили и поехали. Выключить — 10сек удерживать кнопку.
  Тач в зоне timer переключает активный таймер. Долгий тап обнуляет выбранный таймер.
  Тач в зоне trip переключает между поездкой 1 и поездкой 2. Долгий тап обнуляет выбранный поездку, а также сбрасывает максимальную скорость и среднюю скорость в этой поездке. Общий одометр считает всегда и сохраняет значение каждые 100м в энергонезависимую память. 
 Тач на область уровня заряда батареи переключает отображение с % на V.
 Тач на область гаечного ключа открывает страницу настроек:
1. Выбор темы — светлая, темная или автоматическое переключение по датчику освещенности.
2. Длина окружности колеса, стартовое значение 2275мм, шаг изменения - 5 мм. Либо можно в коде  сразу вписать нужное значение — найти строку float wheelCircumferenceMm=2275 .
3. Количество магнитов установленных на колесе. Чем больше магнитов тем плавнее идет подсчет скорости. Важное правило — если магнитов больше одного, они должны располагаться через одинаковое количество спиц.
4. Установка часов
5. Установка минут . Важный момент — установка даты происходит в момент компиляции прошивки, поэтому компиляция и прошивка должна происходить в один день.
6. Выбор системы исчисления — km, km/h, °С, mmHg – mi, mph, °F, inHg


Как точно измерить длину окружности колеса — Ставите велосипед и колесо таким образом, чтобы ниппель был внизу около земли. На земле ставите черту. Катите велосипед вперед на один оборот колеса, пока ниппель снова не окажется возле земли — делаете вторую черту. Обычной рулеткой измеряете расстояние между двумя чертами.
