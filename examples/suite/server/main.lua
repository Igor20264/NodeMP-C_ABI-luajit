-- Объёмный тест. Чат: /suite start | stop | status
-- Спавна нет. Счёт в фоне уходит на ядро LuaJIT.

local HOST = "LuaJIT"
local HAS_BACKGROUND = true
local STEP_SECONDS = 10
local FRAME_MS = 100
local STALL_MS = 250
local JOB_ITERS = 200000
local BURST_N = 200
local BURST_ITERS = 1000

local STEPS = {
    { name = "пустой кадр", kind = "idle", n = 0 },
    { name = "чтение 10", kind = "read", n = 10 },
    { name = "чтение 100", kind = "read", n = 100 },
    { name = "чтение 500", kind = "read", n = 500 },
    { name = "счёт в кадре 10", kind = "inline", n = 10 },
    { name = "счёт в кадре 100", kind = "inline", n = 100 },
    { name = "счёт в кадре 500", kind = "inline", n = 500 },
    { name = "счёт в фоне 10", kind = "back", n = 10 },
    { name = "счёт в фоне 100", kind = "back", n = 100 },
    { name = "счёт в фоне 500", kind = "back", n = 500 },
    { name = "пачка", kind = "burst", n = BURST_N },
    { name = "перекрытие", kind = "overlap", n = 3000 },
    { name = "ожидание", kind = "wait", n = 40 },
}

local function burnJob(args)
    local started = os.clock()
    local total = 0
    local passes = args.passes or 0
    local n = args.iters or 0
    for _ = 1, passes do
        local sum = 0
        for i = 0, n - 1 do
            sum = sum + i
        end
        total = total + sum
    end
    return { ms = (os.clock() - started) * 1000, total = total }
end

local function now()
    return node.uptime()
end

local function clock()
    return os.clock()
end

local function say(text)
    node.log("%s", text)
end

local function readOnce()
    node.snapshot()
    local text = node.players()
    if type(text) ~= "string" then
        return
    end
    for id in text:gmatch('"id"%s*:%s*(%-?%d+)') do
        node.player_name(tonumber(id))
    end
end

local function every(ms, fn)
    return node.every(ms, fn)
end

local function cancel(id)
    if id then
        node.cancel(id)
    end
end

local function startJob(passes, iters, done)
    node.job(burnJob, { passes = passes, iters = iters }, function(result, err)
        if err then
            done(0, err)
            return
        end
        local ms = 0
        if type(result) == "table" then
            ms = tonumber(result.ms) or 0
        elseif type(result) == "number" then
            ms = result
        end
        done(ms, nil)
    end)
end

local function startWait(n, done)
    for _ = 1, n do
        node.after(50, function()
            done()
        end)
    end
end

local running = false
local runId = 0
local serial = 0
local busy = false
local timerId = nil
local stepIndex = 1
local phase = nil
local rows = {}
local startedAt = 0
local deadline = 0
local nextBeat = 0
local overlapArmed = false
local waitArmed = false
local ticks = 0
local skipped = 0
local reads = 0
local readMs = 0
local readMax = 0
local workerMs = 0
local workerMax = 0
local jobsSent = 0
local jobsDone = 0
local jobErrors = 0
local jobsRejected = 0
local burnMs = 0
local burnMax = 0
local stallCount = 0
local waitsDone = 0

local function resetPhase()
    busy = false
    overlapArmed = false
    waitArmed = false
    ticks = 0
    skipped = 0
    reads = 0
    readMs = 0
    readMax = 0
    workerMs = 0
    workerMax = 0
    jobsSent = 0
    jobsDone = 0
    jobErrors = 0
    jobsRejected = 0
    burnMs = 0
    burnMax = 0
    stallCount = 0
    waitsDone = 0
end

local function dueTicks()
    local elapsed = now() - startedAt
    if elapsed < 0 then
        elapsed = 0
    end
    local due = math.floor(elapsed * 10 + 0.5)
    if due < ticks then
        due = ticks
    end
    local cap = STEP_SECONDS * 10
    if due > cap then
        due = cap
    end
    return due
end

local function counts()
    local kind = phase.kind
    local due = STEP_SECONDS * 10
    if kind == "back" then
        return jobsDone, due
    elseif kind == "burst" then
        return jobsDone, ticks * BURST_N
    elseif kind == "overlap" then
        return jobsDone, 1
    elseif kind == "wait" then
        return waitsDone, phase.n
    end
    return ticks, due
end

local function picture()
    local read = ticks > 0 and (readMs / ticks) or 0
    local burn = jobsDone > 0 and (burnMs / jobsDone) or 0
    local doneN, planN = counts()
    return workerMax, read, burn, doneN, planN
end

local function judge(frame)
    local due = dueTicks()
    local missed = due - ticks
    if missed < 0 then
        missed = 0
    end
    local missRatio = due > 0 and (missed / due) or 0
    if stallCount > 0 or frame >= STALL_MS or jobErrors > 0 or jobsRejected > 0 then
        return "плохо"
    end
    if frame > FRAME_MS or missRatio > 0.2 then
        return "на пределе"
    end
    local burn = jobsDone > 0 and (burnMs / jobsDone) or 0
    if burn > FRAME_MS or (ticks > 0 and skipped / ticks > 0.2) then
        return "на пределе"
    end
    return "хорошо"
end

local function lineFor(prefix)
    local frame, read, burn, doneN, planN = picture()
    local text = string.format(
        "%s | %s | кадр %.0f мс | чтение %.0f мс | счёт %.0f мс | сделано %d из %d | замираний %d | %s",
        prefix, phase.name, frame, read, burn, doneN, planN, stallCount, judge(frame)
    )
    if phase.kind == "back" and not HAS_BACKGROUND then
        text = text .. " | фона нет"
    end
    return text
end

local function summary()
    say("сводка " .. HOST)
    for i = 1, #rows do
        say(rows[i])
    end
end

local beginStep
local finishStep

beginStep = function()
    phase = STEPS[stepIndex]
    serial = serial + 1
    resetPhase()
    startedAt = now()
    deadline = startedAt + STEP_SECONDS
    nextBeat = startedAt + 1
    say("старт | " .. phase.name)
end

finishStep = function()
    local text = lineFor("итог")
    rows[#rows + 1] = text
    say(text)
    stepIndex = stepIndex + 1
    if stepIndex > #STEPS then
        running = false
        if timerId then
            cancel(timerId)
            timerId = nil
        end
        summary()
        return
    end
    beginStep()
end

local function onTick()
    if not running or not phase then
        return
    end
    if now() >= deadline then
        finishStep()
        return
    end
    local started = clock()
    local kind = phase.kind
    local n = phase.n
    local token = serial
    if kind == "read" then
        local readStarted = clock()
        for _ = 1, n do
            readOnce()
        end
        local spentRead = (clock() - readStarted) * 1000
        readMs = readMs + spentRead
        if spentRead > readMax then
            readMax = spentRead
        end
        reads = reads + n
    elseif kind == "inline" then
        local result = burnJob({ passes = n, iters = JOB_ITERS })
        local ms = result.ms or 0
        burnMs = burnMs + ms
        if ms > burnMax then
            burnMax = ms
        end
        jobsSent = jobsSent + 1
        jobsDone = jobsDone + 1
    elseif kind == "back" then
        if busy then
            skipped = skipped + 1
        else
            busy = true
            local ok, err = pcall(startJob, n, JOB_ITERS, function(ms, jobErr)
                if token ~= serial then
                    return
                end
                busy = false
                jobsDone = jobsDone + 1
                if jobErr then
                    jobErrors = jobErrors + 1
                    say(tostring(jobErr))
                    return
                end
                burnMs = burnMs + (ms or 0)
                if (ms or 0) > burnMax then
                    burnMax = ms or 0
                end
            end)
            if ok then
                jobsSent = jobsSent + 1
            else
                busy = false
                jobsRejected = jobsRejected + 1
                say(tostring(err))
            end
        end
    elseif kind == "burst" then
        for _ = 1, BURST_N do
            local ok, err = pcall(startJob, 1, BURST_ITERS, function(ms, jobErr)
                if token ~= serial then
                    return
                end
                jobsDone = jobsDone + 1
                if jobErr then
                    jobErrors = jobErrors + 1
                    return
                end
                burnMs = burnMs + (ms or 0)
                if (ms or 0) > burnMax then
                    burnMax = ms or 0
                end
            end)
            if ok then
                jobsSent = jobsSent + 1
            else
                jobsRejected = jobsRejected + 1
                say(tostring(err))
                break
            end
        end
    elseif kind == "overlap" then
        if not overlapArmed then
            overlapArmed = true
            local ok, err = pcall(startJob, n, JOB_ITERS, function(ms, jobErr)
                if token ~= serial then
                    return
                end
                jobsDone = jobsDone + 1
                if jobErr then
                    jobErrors = jobErrors + 1
                    say(tostring(jobErr))
                    return
                end
                burnMs = burnMs + (ms or 0)
                if (ms or 0) > burnMax then
                    burnMax = ms or 0
                end
            end)
            if ok then
                jobsSent = jobsSent + 1
            else
                jobsRejected = jobsRejected + 1
                say(tostring(err))
            end
        end
    elseif kind == "wait" then
        if not waitArmed then
            waitArmed = true
            startWait(n, function()
                if token ~= serial then
                    return
                end
                waitsDone = waitsDone + 1
            end)
        end
    end
    local spent = (clock() - started) * 1000
    workerMs = workerMs + spent
    if spent > workerMax then
        workerMax = spent
    end
    if spent >= STALL_MS then
        stallCount = stallCount + 1
    end
    ticks = ticks + 1
    if now() >= nextBeat then
        nextBeat = now() + 1
        say(lineFor("тик"))
    end
end

local function start()
    if timerId then
        cancel(timerId)
        timerId = nil
    end
    runId = runId + 1
    serial = serial + 1
    running = true
    rows = {}
    stepIndex = 1
    beginStep()
    timerId = every(100, onTick)
    say(HOST .. " | /suite start | stop | status")
end

local function stop()
    if not running then
        say("idle | /suite start")
        return
    end
    running = false
    serial = serial + 1
    if timerId then
        cancel(timerId)
        timerId = nil
    end
    say("стоп | " .. (phase and phase.name or ""))
    summary()
end

local function status()
    if running and phase then
        say(lineFor("тик"))
    else
        say("idle | /suite start")
    end
end

local function words(line)
    local out = {}
    for word in tostring(line):gmatch("%S+") do
        out[#out + 1] = word
    end
    return out
end

local function handle(line)
    local parts = words(line)
    if #parts == 0 then
        return false
    end
    local head = string.lower(parts[1])
    if head ~= "/suite" and head ~= "suite" then
        return false
    end
    local cmd = parts[2] and string.lower(parts[2]) or "help"
    if cmd == "start" then
        start()
    elseif cmd == "stop" then
        stop()
    elseif cmd == "status" then
        status()
    else
        say("/suite start | stop | status")
    end
    return true
end

local function chatText(data)
    if type(data) ~= "string" then
        return ""
    end
    if data:sub(1, 1) ~= "{" then
        return data
    end
    local inner = data:match('"text"%s*:%s*"(.-)"')
    if inner then
        return (inner:gsub('\\"', '"'))
    end
    return data
end

node.on("chatMessageRequest", function(_, _, data)
    handle(chatText(data))
end)

node.on("resourceUnload", function()
    running = false
    if timerId then
        cancel(timerId)
        timerId = nil
    end
end)

say(HOST .. " idle | /suite start | stop | status")
