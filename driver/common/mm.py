#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Исправление ошибок в заголовочных файлах драйвера W522A для Linux-ядра 6.12.81
Текущая проблема: в wifi_sdio_cfg_addr.h используется BIT(n), но макрос не определён.
Скрипт добавляет корректное определение макроса.
"""

import os
import re

# Имя файла, который нужно исправить
TARGET_FILE = "wifi_sdio_cfg_addr.h"

# Определение, которое нужно вставить
BIT_DEFINITION = """
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
"""

def fix_bit_macro(filepath):
    if not os.path.isfile(filepath):
        print(f"Файл не найден: {filepath}")
        return False

    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    # Проверяем, есть ли уже определение BIT или подключение linux/bits.h
    for line in lines:
        if line.strip().startswith('#include <linux/bits.h>') or \
           re.search(r'#define\s+BIT\(', line):
            print(f"В {filepath} уже есть определение BIT или #include <linux/bits.h>. Пропускаем.")
            return False

    # Ищем место для вставки (после последнего #include или после стражей)
    insert_index = -1
    for i, line in enumerate(lines):
        # Если нашли строку #define __WIFI_SDIO_CFG_ADDR_H__ (после guard)
        if re.match(r'#define\s+__WIFI_SDIO_CFG_ADDR_H__', line.strip()):
            insert_index = i + 1
            break
        # Либо после последнего #include (если есть)
        if line.strip().startswith('#include') and i > insert_index:
            insert_index = i + 1

    # Если не нашли подходящее место, вставляем после #define guard (самое начало)
    if insert_index == -1:
        for i, line in enumerate(lines):
            if re.match(r'#define\s+\w+_H__', line.strip()):
                insert_index = i + 1
                break
        if insert_index == -1:
            insert_index = 2  # после первых двух строк (обычно #ifndef и #define)

    # Вставляем определение
    lines.insert(insert_index, BIT_DEFINITION)

    # Записываем обратно
    with open(filepath, 'w', encoding='utf-8') as f:
        f.writelines(lines)

    print(f"Успешно исправлен: {filepath}")
    return True

def main():
    # Ищем файл в текущей директории и поддиректориях (на случай, если он не в корне)
    found = False
    for root, dirs, files in os.walk('.'):
        if TARGET_FILE in files:
            found = True
            fix_bit_macro(os.path.join(root, TARGET_FILE))
            break
    if not found:
        print(f"Файл {TARGET_FILE} не найден. Убедитесь, что скрипт запущен в корне драйвера.")

if __name__ == "__main__":
    main()