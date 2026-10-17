# LuaJIT для NodeMP

Языковой модуль сервера NodeMP, сборка LuaJIT-v2. Кладётся в `modules/ljhost.dll` и регистрирует ресурсы `type = "luajit-v2"`. Старое имя `type = "luajit"` ведёт в тот же хост. Скрипт видит глобальные `node` и `resource`. Это отдельный репозиторий от хостов CPython и MicroPython.

Интерпретатор вшит в `ljhost.dll`. Отдельный `lua51.dll` в `modules/` класть не нужно: сервер примет его за плагин и откажется грузить.

Заголовок SDK лежит в `sdk/node.h` (ABI 2.8). Сервер 1.12.0 говорит на ABI 2.7, поэтому в логе будет `NEWER SDK (2.8 vs 2.7)`. Это Lua 5.1, не 5.4.

Положите `ljhost.dll` в `modules/` и перезапустите сервер. Пример ресурса — `examples/hello`.

```toml
name = "hello-jit"
type = "luajit-v2"

[server]
main = "server/main.lua"
```

## Отдельное ядро

`node.job(fn, arg, done)` ставит вызов в очередь одного потока. Поток закреплён за одним логическим процессором.

- По умолчанию это третье с конца, чтобы не делить ядро с Python (последнее) и MicroPython (предпоследнее). `NODEMP_LUAJIT_CORE=9` выбирает другое, `last` берёт последнее.
- Пока `NODEMP_LUAJIT_RESERVE` не равен `0`, у остальных потоков процесса этот процессор снимается с маски. Обход повторяется раз в пять секунд. Поток, у которого этот процессор единственный, не трогается.
- Очередь одна. Поток спит, пока задач нет.
- `fn` получает один аргумент и возвращает одно значение. `done(result, err)` возвращается на рабочий поток. Из задания этот callback ждать нельзя.
- Функция без внешних локальных переменных считается в отдельном состоянии ядра. Игровой тик в это время не ждёт замок. Числа, строки и таблицы аргумента и результата копируются. `node` внутри такого задания нет.
- Функция, которая замыкает локальные переменные, считается по-старому, в состоянии ресурса и под замком. Обычный Lua сервера в обоих случаях идёт дальше.

```lua
node.job(function(n)
    local total = 0
    for i = 0, n - 1 do total = total + i end
    return total
end, 1000000, function(result, err)
    node.log(tostring(result or err))
end)
```

`node.core_info()` возвращает `index`, `processors`, `pinned`.

## Что есть у скрипта

`node.log` принимает строку формата, как `string.format`. Рядом `node.warn`, `node.error` и те же имена внутри `node.log.warn` / `node.log.error`.

`node.on(name, fn)` подписывает на событие сервера. Встроенное событие получает `player_id`. Событие машины и `resourceUnload` получают `player_id`, `global_id`, `data`. Запрос (`chatMessageRequest` и остальные `*Request`) получает те же три аргумента: возврат `false` или строки запрещает действие, строка записывается как причина.

`node.every(мс, fn)` и `node.after(мс, fn)` возвращают id для `node.cancel`.

Чтение мира: `node.snapshot()`, `node.players()`, `node.player_name(id)`, `node.player_count()`, `node.vehicle_count()`, `node.uptime()`, `node.handler_stats(reset)`. Машины: `node.spawn_vehicle(config_json, owner)`, `node.delete_vehicle(id)`. `node.version()` — строка LuaJIT.

`resource.name`, `resource.dir` и `resource.trusted` читаются как поля. Доверие задаётся в `server.toml`:

```toml
[resources]
trusted = ["hello-jit"]
```

Без доверия нет `ffi`, `io` и `debug`, а `os` остаётся с `clock`, `time`, `difftime` и `date`. Доверенный ресурс получает полный LuaJIT, включая `ffi`.

## Сборка

Нужны gcc из MinGW-w64 (`C:\mingw64\bin\gcc.exe`) и Git. Исходники LuaJIT makefile забирает сам, в репозиторий они не входят.

```text
mingw32-make
```

Готовый `ljhost.dll` кладётся в `modules/`.
