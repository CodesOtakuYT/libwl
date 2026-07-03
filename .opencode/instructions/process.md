# Development Process

Every feature or fix follows this sequence:

## 1. Research
Understand the problem space before writing code.
- Read official specs, protocol XML docs, and reference implementations (Weston, gamescope, etc.)
- Search the codebase for existing patterns and conventions
- Identify all the "unusual" aspects that differ from intuition

## 2. Prototype
Write the simplest working version first.
- Prioritize correctness over elegance
- Use `assert()` and simple patterns to keep focus on the pipeline, not error paths
- Document assumptions and unknowns as you go

## 3. Cross-Reference
Validate against real implementations.
- Compare against Weston's reference clients (`simple-shm.c`, `simple-dmabuf-egl.c`, etc.)
- Check protocol specs for wire format correctness (modifier hi/lo order, etc.)
- Look for bugs that compile clean but fail semantically (wrong apiVersion, wrong pixel format doc, non-thread-safe call)

## 4. Verify & Iterate
Build, run, and fix regressions.
- `ninja -C buildDir` — must compile with zero warnings
- Each change must be independently verifiable
- If a fix introduces a new warning, fix the warning or revert

## 5. Document
Explain the *why*, not just the *what*.
- Every design decision gets a rationale in the tutorial or a comment
- Legacy patterns that differ from our approach go in the "Wild Code" chapter
- AI mistakes are preserved — they teach more than successes do

## For AI Agents
When asked to investigate a topic:
1. Read the relevant tutorial chapter first (it explains the mental model)
2. Read the libwl source for the real implementation
3. Cross-reference with Weston's reference clients
4. Report differences, don't silently diverge
