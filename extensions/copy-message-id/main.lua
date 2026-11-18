-- Copy Message ID: adds a message context menu item that copies the message id to the clipboard.
ui.contextMenu.add({
  id = "copy-message-id",
  label = "Copy Message ID",
  target = "message",
  action = function(ctx)
    clipboard.write(ctx.message_id)
  end,
})
