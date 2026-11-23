# DriveLink guide writing rules

Write for someone with the released packages and no prior embedded or ROS experience. Use plain English, second person and short sentences. Give each walkthrough one task, its prerequisites and an observable outcome. Organize reference pages by subject and troubleshooting pages by symptom.

- Give each step an observable result. Copy labels and messages from the
  running product. Explain each term on first use or link to the glossary.
- Put copyable commands in fenced blocks with no prompt or output. Try them
  with the shipped packages. Keep Windows/Ubuntu and Lyrical/Jazzy choices
  together using the guide’s tabs. Use a Markdown table for generated reference
  rows and a short, meaningful link label.
- Put a safety callout before anything that can move a motor. Include wheels
  off the ground, a current-limited supply, a low duty cap and a hand on the
  power switch. Use a warning before anything that erases settings.
- Add GUI screenshots through the scenario list. Write alt text about the
  information the picture conveys. The build pairs light and dark images.
- Add pages to navigation.json and keep page paths and headings stable. Keep
  scripts, styles, fonts and images local. Release assets and official
  documentation can be links.

From the repository root, run `python tools/validate.py --only guide` for routine validation. To preview the built guide for visual or interactive review, run `python tools/guide.py serve`. See [Development](../docs/development.md) for setup and release inputs.
