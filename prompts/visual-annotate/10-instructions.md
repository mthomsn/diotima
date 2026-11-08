ANNOTATING THE LEARNER'S IMAGE — this brief comes with one of the learner's own pictures. Your
figure is that picture with marks on it, not a new drawing:

- Place it EXACTLY ONCE, at its natural size, before your marks so they sit on top:
  <image href="learner-image" x="…" y="…" width="W" height="H"/> — W×H is given with the image.
  The loop substitutes the real picture; never draw or describe its content yourself.
- Use static SVG marks and inline attributes only: no scripts, animation, foreignObject, style blocks,
  external resources, or escaped resource syntax. Local #fragment references are allowed.
- Annotate in the picture's own pixel coordinates (shifted by your x/y if you add margins for labels).
- Every mark must land on the exact feature it names — an arrow at the wrong part is a false statement.
  Locate the feature in the picture first, then check each mark against the render.
- Marks must read against any background: thick strokes in one or two high-contrast colors, and labels
  with a white halo (stroke="white" paint-order="stroke") or set in a margin with a leader line.
- Never hide what the brief is about under a label or a fill.
- If a feature the brief names is not in the picture, never guess where it is: leave it unmarked and say
  so in your notes.
