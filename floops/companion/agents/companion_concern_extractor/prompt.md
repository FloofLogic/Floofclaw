Decide whether the selected user input belongs to an ongoing concern worth tracking.

Return exactly one JSON object.

Allowed outputs:

Create a new concern:
{"affair":{"action":"create","manage":"<short human description>","note_text":"<what this turn adds>","status":"active"}}

Link this turn to an existing concern without changing it:
{"affair":{"action":"link","affair_id":"...","manage":"","note_text":"","status":"active"}}

Update an existing concern:
{"affair":{"action":"update","affair_id":"...","manage":"<updated description>","note_text":"<what this turn adds>","status":"active"}}

No concern:
{"affair":{"action":"none","manage":"","note_text":"","status":"active"}}

Rules:
- Concerns are durable threads the user is navigating: an ongoing
  situation, a recurring theme, a life-stage transition, a worry that
  keeps coming back, a relationship under tension, a health issue, a
  decision being weighed over time.
- A concern is not a feeling. "I'm tired" is not a concern. "I've been
  exhausted for weeks because of the new baby" is.
- A concern is not a complaint about today. "Traffic was awful" is
  not a concern. "My commute has been wearing me down since the move"
  is.
- Most turns are not concerns. Default to none.
- Use create only when the user is bringing up something with a
  clear future shape — something where checking in later would
  actually help, or where future turns would naturally connect back.
- Use link when the turn belongs to an existing concern but adds no
  durable new summary.
- Use update only when the turn changes the concern's durable
  summary, title, or direction (a resolution, a new development,
  a shift in stance).
- Most follow-up turns should link, not update.
- Runtime owns IDs for new concerns.
- For link or update, use an affair_id from affairs.active.
- Keep `manage` short and human, not clinical. "Job change in
  spring" not "Career transition concern". "Mom's drinking" not
  "Familial alcohol substance issue".
- `note_text` is what THIS turn adds — a one-sentence observation
  the user can read back later ("user mentioned dad called and
  apologized"). Leave empty on `link` and `none`.
- Do not answer the user.
- Do not explain.
- Do not include extra keys.

Input:
{{json}}
