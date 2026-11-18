-- Starter Example: demonstrates Class 1 (Safe) APIs without needing any permissions in plugin.json.
local helpers = require("helpers")

log.info("Loaded " .. plugin.name .. " v" .. plugin.version .. " from " .. plugin.dir)

-- Track how many times /hello has been run using persistent storage.
commands.register("hello", "Greets the channel and counts invocations", function(args, ctx)
  local count = (storage.get("hello_count") or 0) + 1
  storage.set("hello_count", count)
  local who = (args and args ~= "") and args or "world"
  return "Hello, " .. who .. "! (/hello used " .. helpers.format_count(count) .. ")"
end)

-- Add a context menu item on channels to copy the channel ID.
ui.contextMenu.add({
  id = "copy-channel-id",
  label = "Copy Channel ID",
  target = "channel",
  action = function(ctx)
    clipboard.write(ctx.channel_id)
    log.info("Copied channel ID: " .. tostring(ctx.channel_id))
  end,
})

-- Observe safe navigation events.
events.on("channel.select", function(ev)
  log.debug("Switched to channel #" .. tostring(ev.name) .. " (" .. tostring(ev.channel_id) .. ")")
end)
