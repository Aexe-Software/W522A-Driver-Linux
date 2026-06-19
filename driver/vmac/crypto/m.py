#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Исправление недостающих макросов в криптографических файлах драйвера W522A.
- Добавляет макрос FREE (вызов bin_clear_free)
- Добавляет макрос ERROR_DEBUG_OUT (вызов wpa_printf)
- Инициализирует массив zero в aes-siv.c
"""

import os
import re

# Конфигурация
HEADER_FILE = "aml_crypto_wrap.h"
AES_SIV_FILE = "aes-siv.c"

# Определения, которые нужно добавить в заголовочный файл
MACROS_TO_ADD = """
/*
 * Недостающие макросы, добавленные для совместимости
 */
#ifndef FREE
#define FREE(ptr, tag) bin_clear_free(ptr, 0)
#endif

#ifndef ERROR_DEBUG_OUT
#define ERROR_DEBUG_OUT(fmt, ...) wpa_printf(_MSG_ERROR_, fmt, ##__VA_ARGS__)
#endif
"""

def add_macros_to_header(header_path):
    """Добавляет определения макросов в aml_crypto_wrap.h, если их нет."""
    if not os.path.isfile(header_path):
        print(f"Файл не найден: {header_path}")
        return False

    with open(header_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Проверяем, есть ли уже FREE или ERROR_DEBUG_OUT
    if re.search(r'#define\s+FREE\(', content) and \
       re.search(r'#define\s+ERROR_DEBUG_OUT\(', content):
        print("Макросы FREE и ERROR_DEBUG_OUT уже определены. Пропускаем.")
        return False

    # Ищем место для вставки (после последнего #include или перед концом файла)
    insert_pos = -1
    lines = content.splitlines(keepends=True)
    for i, line in enumerate(lines):
        if line.strip().startswith('#include'):
            insert_pos = i + 1  # после последнего include
        elif line.strip().startswith('//') or line.strip().startswith('/*'):
            continue
        elif line.strip().startswith('#define') and insert_pos == -1:
            # если не нашли include, вставляем перед первым #define
            insert_pos = i
            break

    if insert_pos == -1:
        insert_pos = len(lines)  # в конец файла

    # Вставляем макросы
    lines.insert(insert_pos, MACROS_TO_ADD)
    with open(header_path, 'w', encoding='utf-8') as f:
        f.writelines(lines)

    print(f"Макросы добавлены в {header_path}")
    return True

def fix_zero_array_in_aes_siv(siv_path):
    """Заменяет 'static const u8 zero[AES_BLOCK_SIZE];' на инициализированную версию."""
    if not os.path.isfile(siv_path):
        print(f"Файл не найден: {siv_path}")
        return False

    with open(siv_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Ищем объявление zero
    pattern = r'static const u8 zero\[AES_BLOCK_SIZE\];'
    replacement = r'static const u8 zero[AES_BLOCK_SIZE] = { 0 };'
    if re.search(pattern, content):
        new_content = re.sub(pattern, replacement, content)
        if new_content != content:
            with open(siv_path, 'w', encoding='utf-8') as f:
                f.write(new_content)
            print(f"Массив zero в {siv_path} явно инициализирован.")
            return True
        else:
            print(f"Массив zero уже инициализирован в {siv_path}.")
            return False
    else:
        print(f"Объявление zero не найдено в {siv_path} или уже имеет инициализацию.")
        return False

def main():
    # Ищем заголовочный файл в текущей директории и поддиректориях
    header_found = False
    for root, dirs, files in os.walk('.'):
        if HEADER_FILE in files:
            header_found = True
            add_macros_to_header(os.path.join(root, HEADER_FILE))
            break
    if not header_found:
        print(f"Внимание: файл {HEADER_FILE} не найден. Убедитесь, что скрипт запущен в корне драйвера.")

    # Ищем aes_siv.c
    siv_found = False
    for root, dirs, files in os.walk('.'):
        if AES_SIV_FILE in files:
            siv_found = True
            fix_zero_array_in_aes_siv(os.path.join(root, AES_SIV_FILE))
            break
    if not siv_found:
        print(f"Внимание: файл {AES_SIV_FILE} не найден.")

    print("\nИсправление завершено. Попробуйте собрать драйвер снова.")

if __name__ == "__main__":
    main()