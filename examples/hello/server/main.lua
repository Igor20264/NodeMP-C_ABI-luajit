node.log(resource.name .. " on " .. node.version())

node.job(function(n)
    local total = 0
    for i = 0, n - 1 do
        total = total + i
    end
    return total
end, 1000000, function(result, err)
    local info = node.core_info()
    node.log("sum " .. tostring(result or err) .. " on core " .. tostring(info.index))
end)
