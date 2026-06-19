#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Исправление критических ошибок в w1_sdio.c для ядра 6.12.81
- Устранение use-after-free для scatter-структур
- Очистка массива sdio_func_if при remove
- Атомарный счётчик shutdown_i
- Пересоздание sdio_func_0 при каждом probe
"""

import os
import re
import sys

FILE = "w1_sdio.c"
HEADER = "w1_sdio.h"

def fix_w1_sdio_c(filepath):
    if not os.path.isfile(filepath):
        print(f"Файл {filepath} не найден")
        return False

    with open(filepath, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    modified = False
    new_lines = []
    i = 0

    # Патч 1: избавиться от статической sdio_func_0, создать локальную в probe
    # Ищем место объявления static struct sdio_func sdio_func_0;
    # и заменяем на пустой комментарий, а в probe вставляем инициализацию на стеке.
    static_func0_line = -1
    for idx, line in enumerate(lines):
        if re.search(r'static struct sdio_func sdio_func_0;', line):
            static_func0_line = idx
            break

    if static_func0_line != -1:
        # Заменяем строку на комментарий
        lines[static_func0_line] = "/* static struct sdio_func sdio_func_0; -- moved to probe */\n"
        modified = True
        print("[*] Удалено статическое объявление sdio_func_0")
    else:
        # Возможно, уже исправлено
        print("[?] Объявление static sdio_func_0 не найдено, пропускаем")

    # Патч 2: в probe для func->num == 1 добавить локальную инициализацию
    # Ищем строку if (func->num == 1)
    probe_insert_idx = -1
    for idx, line in enumerate(lines):
        if re.search(r'if\s*\(\s*func->num\s*==\s*1\s*\)', line):
            probe_insert_idx = idx + 1  # после этой строки
            break

    if probe_insert_idx != -1:
        # Находим отступ (пробелы/табы)
        indent = ""
        while probe_insert_idx < len(lines) and lines[probe_insert_idx].strip() == '':
            probe_insert_idx += 1
        if probe_insert_idx < len(lines):
            m = re.match(r'^(\s+)', lines[probe_insert_idx])
            if m:
                indent = m.group(1)
        insertion = [
            f"{indent}struct sdio_func sdio_func_0_local = {{ .num = 0, .card = func->card }};\n",
            f"{indent}w1_g_w1_hwif_sdio.sdio_func_if[0] = &sdio_func_0_local;\n",
        ]
        # Вставляем после if
        for off, line in enumerate(insertion):
            lines.insert(probe_insert_idx + off, line)
        modified = True
        print("[*] В probe добавлена локальная инициализация sdio_func_0")
    else:
        print("[!] Не найден if (func->num == 1) — возможно, код уже изменён")

    # Патч 3: в probe для последней функции сбросить scatter_enabled и scat_req
    last_func_pattern = r'if\s*\(\s*func->num\s*!=\s*FUNCNUM_SDIO_LAST\s*\)'
    last_func_idx = -1
    for idx, line in enumerate(lines):
        if re.search(last_func_pattern, line):
            last_func_idx = idx
            # это условие, после него return 0; нам нужно вставить после блока else
            # ищем фигурную скобку и после неё вставляем сброс
            break

    if last_func_idx != -1:
        # Ищем место после else (где функция возвращает 0)
        # Проще: найти строку "return ret;" после условия и вставить перед ней
        for idx in range(last_func_idx, len(lines)):
            if re.search(r'return ret;', lines[idx]) and 'w1_g_w1_hwif_sdio' in lines[idx-1]:
                indent = re.match(r'^(\s+)', lines[idx]).group(1)
                reset_lines = [
                    f"{indent}/* v28z78: reset scatter state on re-probe */\n",
                    f"{indent}w1_g_w1_hwif_sdio.scatter_enabled = false;\n",
                    f"{indent}w1_g_w1_hwif_sdio.scat_req = NULL;\n",
                ]
                for off, line in enumerate(reset_lines):
                    lines.insert(idx + off, line)
                modified = True
                print("[*] В probe добавлен сброс scatter_enabled/scat_req")
                break
    else:
        print("[!] Не найдено условие func->num != FUNCNUM_SDIO_LAST — пропускаем")

    # Патч 4: в remove добавить очистку sdio_func_if и вызов hi_cleanup_scat
    # Ищем начало функции aml_w1_sdio_remove
    remove_start = -1
    for idx, line in enumerate(lines):
        if re.search(r'static void\s+aml_w1_sdio_remove\s*\(', line):
            remove_start = idx
            break

    if remove_start != -1:
        # Ищем место перед sdio_release_host или в конце, после sdio_disable_func
        # Добавим в самом начале после WRITE_ONCE, но перед sdio_claim_host
        # Лучше добавить после sdio_disable_func, перед sdio_release_host
        for idx in range(remove_start, len(lines)):
            if re.search(r'sdio_disable_func\s*\(\s*func\s*\)\s*;', lines[idx]):
                # Вставляем после этой строки
                indent = re.match(r'^(\s+)', lines[idx]).group(1)
                cleanup_lines = [
                    f"{indent}/* v28z78: clear function pointer and free scatter resources */\n",
                    f"{indent}if (func->num == FUNCNUM_SDIO_LAST) {{\n",
                    f"{indent}    if (w1_g_w1_hif_ops.hi_cleanup_scat)\n",
                    f"{indent}        w1_g_w1_hif_ops.hi_cleanup_scat();\n",
                    f"{indent}    w1_g_w1_hwif_sdio.scatter_enabled = false;\n",
                    f"{indent}    w1_g_w1_hwif_sdio.scat_req = NULL;\n",
                    f"{indent}}}\n",
                    f"{indent}w1_g_w1_hwif_sdio.sdio_func_if[func->num] = NULL;\n",
                ]
                for off, line in enumerate(cleanup_lines):
                    lines.insert(idx + 1 + off, line)
                modified = True
                print("[*] В remove добавлена очистка sdio_func_if и вызов hi_cleanup_scat")
                break
    else:
        print("[!] Функция aml_w1_sdio_remove не найдена")

    # Патч 5: сделать shutdown_i атомарным (atomic_t)
    # Найти объявление unsigned int shutdown_i = 0;
    for idx, line in enumerate(lines):
        if re.search(r'unsigned\s+int\s+shutdown_i\s*=\s*0\s*;', line):
            lines[idx] = "static atomic_t shutdown_i = ATOMIC_INIT(0);\n"
            modified = True
            print("[*] shutdown_i заменён на atomic_t")
            break

    # Заменить все инкременты и проверки shutdown_i
    # Ищем "shutdown_i += 1" и "shutdown_i == 1" и "shutdown_i == 7" и "shutdown_i = 0"
    for idx, line in enumerate(lines):
        if 'shutdown_i += 1' in line:
            lines[idx] = line.replace('shutdown_i += 1', 'atomic_inc(&shutdown_i)')
            modified = True
        if 'shutdown_i == 1' in line:
            lines[idx] = line.replace('shutdown_i == 1', 'atomic_read(&shutdown_i) == 1')
            modified = True
        if 'shutdown_i == 7' in line:
            lines[idx] = line.replace('shutdown_i == 7', 'atomic_read(&shutdown_i) == 7')
            modified = True
        if 'shutdown_i = 0' in line:
            lines[idx] = line.replace('shutdown_i = 0', 'atomic_set(&shutdown_i, 0)')
            modified = True

    # Патч 6: заменить PRINT для ошибок на pr_err
    for idx, line in enumerate(lines):
        if 'PRINT("failed to register sdio driver' in line:
            lines[idx] = line.replace('PRINT', 'pr_err')
            modified = True

    if modified:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.writelines(lines)
        print(f"[+] Файл {filepath} успешно исправлен")
    else:
        print("[=] Файл не нуждается в изменениях или не найдены паттерны")

    return modified

def fix_header(filepath):
    if not os.path.isfile(filepath):
        return False
    # Добавим #include <asm/atomic.h> если его нет (хотя атомарные уже через linux/types.h)
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    if '#include <linux/atomic.h>' not in content and '#include <asm/atomic.h>' not in content:
        # Вставим после #include <linux/version.h>
        new_content = re.sub(r'(#include <linux/version.h>)', r'\1\n#include <linux/atomic.h>', content)
        if new_content != content:
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(new_content)
            print("[+] В заголовочный файл добавлен #include <linux/atomic.h>")
            return True
    return False

def main():
    # Ищем файлы в текущей директории и поддиректориях
    found = False
    for root, dirs, files in os.walk('.'):
        if FILE in files:
            fix_w1_sdio_c(os.path.join(root, FILE))
            found = True
        if HEADER in files:
            fix_header(os.path.join(root, HEADER))
    if not found:
        print(f"Файл {FILE} не найден в текущей иерархии.")
        sys.exit(1)

if __name__ == "__main__":
    main()