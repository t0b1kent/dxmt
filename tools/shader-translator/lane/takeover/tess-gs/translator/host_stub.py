"""tess-gs 14.10.2026: убрать буферы записей из пониженного модуля lower_isoline.

Пониженный модуль = inner-функции тела (hull_main/patch_main/main0 …) + ядро-обёртка, которое читает записи из
буферов set 31, зовёт оркестратор и пишет записи назад. Для object/mesh-обёрток нужны только inner-функции, а
root-ABI SPIRV-Cross отвергает любой ресурс без корневого соответствия. Здесь ядро переписывается в заглушку:
остаются переменные Function/Workgroup, срез аргументов вызова оркестратора, сам вызов и возврат; буферы записей,
их декорации, имена и обращения к ним удаляются. Валидность — spirv-val на выходе.
"""
import struct

OP_NAME, OP_ENTRY, OP_EXEC_MODE, OP_DECORATE, OP_VARIABLE = 5, 15, 16, 71, 59
OP_FUNCTION, OP_FUNCTION_END, OP_LABEL, OP_RETURN, OP_CALL = 54, 56, 248, 253, 57
OP_LOAD, OP_STORE, OP_ACCESS_CHAIN, OP_COPY_MEMORY = 61, 62, 65, 63
OP_BARRIER = (224, 225)
SC_STORAGE_BUFFER, SC_UNIFORM, SC_WORKGROUP, SC_FUNCTION = 12, 2, 4, 7
RESULT_LESS = {OP_ENTRY, OP_EXEC_MODE, OP_DECORATE, OP_NAME, OP_STORE, OP_RETURN, OP_FUNCTION_END, OP_LABEL,
               OP_COPY_MEMORY, 72, 74, 249, 250, 246, 247, 252, 254, 255}  # без результата (OpLabel даёт результат, но без типа)


class Unsupported(ValueError):
    pass


def _parse(data):
    if len(data) < 20 or len(data) % 4 or struct.unpack_from('<I', data, 0)[0] != 0x07230203:
        raise Unsupported('SPIR-V header')
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    ins, i = [], 5
    while i < len(words):
        n, op = words[i] >> 16, words[i] & 0xffff
        if not n or i + n > len(words):
            raise Unsupported('instruction bounds')
        ins.append([op, words[i + 1:i + n]])
        i += n
    return words[:5], ins


def _result_id(op, args):
    """id результата инструкции или None (по типу инструкции, достаточно для тел функций lower)."""
    if op in (OP_LABEL,):
        return args[0]
    if op in RESULT_LESS or op in OP_BARRIER:
        return None
    if op in (OP_VARIABLE, OP_LOAD, OP_ACCESS_CHAIN, OP_CALL) or 17 <= op <= 33 or op in (43, 44, 45, 46, 50, 51, 52,
            79, 80, 81, 82, 83, 84, 124, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,
            143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163,
            164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184,
            185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205,
            206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 12, 14, 63 - 63):
        return args[1] if len(args) > 1 else None
    return args[1] if len(args) > 1 else None


def strip_records(data, entry_name='mr_isoline_compute'):
    header, ins = _parse(data)
    # буферы записей: StorageBuffer-переменные с DescriptorSet 31
    sets = {a[0]: a[2] for op, a in ins if op == OP_DECORATE and len(a) >= 3 and a[1] == 34}
    records = {a[1] for op, a in ins if op == OP_VARIABLE and a[2] == SC_STORAGE_BUFFER and sets.get(a[1]) == 31}
    if not records:
        raise Unsupported('record buffers (set 31) not found')
    entry = [a for op, a in ins if op == OP_ENTRY]
    if len(entry) != 1:
        raise Unsupported('single entry point expected')
    entry_fn = entry[0][1]
    # границы функции-ядра
    start = next(i for i, (op, a) in enumerate(ins) if op == OP_FUNCTION and a[1] == entry_fn)
    end = next(i for i in range(start, len(ins)) if ins[i][0] == OP_FUNCTION_END)
    body = ins[start:end + 1]
    calls = [(i, a) for i, (op, a) in enumerate(body) if op == OP_CALL]
    if len(calls) != 1:
        raise Unsupported('kernel must call the orchestrator exactly once, found %d' % len(calls))
    call_at, call = calls[0]
    # обратный срез по аргументам вызова: инструкции, нужные для их вычисления, без обращений к записям
    produced = {}
    for i, (op, a) in enumerate(body):
        r = _result_id(op, a)
        if r is not None:
            produced[r] = i
    tainted = set(records)
    for i, (op, a) in enumerate(body):
        r = _result_id(op, a)
        if r is None:
            continue
        uses = [x for x in a[2:]] if op not in (OP_VARIABLE,) else [a[3]] if len(a) > 3 else []
        if op == OP_LOAD or op == OP_ACCESS_CHAIN or op == OP_CALL:
            uses = a[2:]
        if any(u in tainted for u in uses):
            tainted.add(r)
    needed, stack = set(), list(call[3:])
    while stack:
        x = stack.pop()
        if x in needed or x not in produced:
            continue
        if x in tainted:
            raise Unsupported('orchestrator argument depends on record buffers: %%%d' % x)
        needed.add(x)
        op, a = body[produced[x]]
        if op == OP_VARIABLE:
            continue
        stack.extend(a[2:])
    # Заглушка — ОДИН блок: OpFunction, первая метка, переменные, срез аргументов, вызов, OpReturn, OpFunctionEnd.
    # Барьеры, ветвления и прочие блоки ядра (чтение/запись записей, lane-0 хвост) отбрасываются.
    labels = [i for i, (op, a) in enumerate(body) if op == OP_LABEL]
    if not labels or call_at < labels[0]:
        raise Unsupported('kernel without an entry block before the call')
    first_block_end = labels[1] if len(labels) > 1 else end - start
    if call_at >= first_block_end:
        raise Unsupported('orchestrator call is not in the entry block')
    keep = [body[0], body[labels[0]]]
    for i in range(labels[0] + 1, call_at):
        op, a = body[i]
        if op == OP_VARIABLE:
            keep.append([op, a[:3]])   # без инициализатора (он мог зависеть от записей)
        else:
            r = _result_id(op, a)
            if r is not None and r in needed:
                keep.append([op, a])
    # переменные функции обязаны стоять в начале первого блока — могут встретиться и после вызова
    for i in range(call_at + 1, end - start):
        op, a = body[i]
        if op == OP_VARIABLE:
            keep.insert(2, [op, a[:3]])
    keep.append(body[call_at])
    keep.append([OP_RETURN, []])
    keep.append([OP_FUNCTION_END, []])
    out = ins[:start] + keep + ins[end + 1:]
    # убрать буферы записей, их декорации и имена; из интерфейса точки входа — их id
    out = [[op, a] for op, a in out if not (op == OP_VARIABLE and a[1] in records)
           and not (op == OP_DECORATE and a[0] in records) and not (op == OP_NAME and a[0] in records)]
    for item in out:
        if item[0] == OP_ENTRY:
            name_words = 0
            # OpEntryPoint model fn "name" iface...: имя — строка; интерфейс после неё
            a = item[1]
            # найти конец строки имени
            k = 2
            while k < len(a):
                w = a[k]
                k += 1
                if any(((w >> (8 * b)) & 0xff) == 0 for b in range(4)):
                    break
            item[1] = a[:k] + [x for x in a[k:] if x not in records]
    words = list(header)
    for op, a in out:
        words.append(((len(a) + 1) << 16) | op)
        words.extend(a)
    return struct.pack('<%dI' % len(words), *words), dict(records=sorted(records), kept=len(keep), was=len(body))


if __name__ == '__main__':
    import sys, json
    src, dst = sys.argv[1:3]
    out, info = strip_records(open(src, 'rb').read())
    open(dst, 'wb').write(out)
    print(json.dumps(info))
