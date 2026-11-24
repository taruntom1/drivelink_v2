# Saving and reverting

If you’re editing a layout or tuning a controller, this page helps you tell when an edit is active and when it will survive a restart.

- **Live edits** can take effect immediately. **Unsaved** means the app has tracked a change since the last save.
- **Pending edits** need a restart before they take effect. On **Layout**, use **Review** to inspect them. **Save and reboot** stores them and restarts the board.
- **Save** stores the current configuration in the board’s flash memory. Verify the values after a restart if you rely on them for the next run.
- **Revert** returns to the last saved configuration. Use **Discard all** on the Layout pending banner when you want to drop the staged edits shown there. Read the confirmation before proceeding. **Discard all** does not undo a **Save**: after you save and then discard, the next start still loads the saved values. To go back, set the old value and save again.

The app prevents configuration saves and restarts while the board is [armed]({{base}}concepts/safety/). If you plan to try several settings, start from a known saved configuration and review each change before saving it.

[Build the layout]({{base}}pc/layout/) to see the review and reboot in context.
