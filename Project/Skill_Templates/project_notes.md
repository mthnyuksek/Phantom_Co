---
name: Project notes
description: Read and write the persistent notes of projects (shared memory).
tools: Read, Write, Edit, Glob
dirs: Projects
min_role: editor
default: true
---
Each project keeps notes that every agent working in it receives with its instructions. Write a note when you learn something that will matter again (a decision, a convention, a result). Keep notes short and factual.
Where: <vault>/Projects/<project_id>/.Phantom/Notes/<note_id>.md
Format: a header between two `---` lines, then the text.
```
---
name: Short title
enabled: true
---
The note text.
```
`<note_id>` is the title in lower case with underscores. Set `enabled: false` to switch a note off instead of deleting it. The project summary is in <vault>/Projects/<project_id>/.Phantom/project.md (`name:` header, summary below).
