---
name: savvycan
description: Work on the SavvyCAN application, its CAN reverse-engineering tools, embedded AI capabilities, OBD-II, UDS, CANopen, raw CAN, payload formatting, and Qt user interface. Use when changing or explaining this SavvyCAN repository.
---

# SavvyCAN

Read `help/ai_chat_skill.md` before changing embedded AI behavior. Treat
`re/aiactionregistry.cpp` as the authoritative native capability catalog and
`help/ai_skills.json` as the natural-language routing manifest.

## Workflow

1. Read the affected workbench implementation and its corresponding help page.
2. Preserve the central AI access, validation, arming and confirmation gates.
3. Add GUI operations as typed native capabilities rather than prompt-only
   conventions or scripting.
4. Put each capability in at least one focused skill in `help/ai_skills.json`.
5. Add or update a case in `help/ai_skill_evaluations.json` for routing changes.
6. Run the skill evaluation test and the applicable Qt build.

## Domain references

- OBD-II: `help/obd2_workbench.md`
- UDS: `help/uds_workbench.md`
- CANopen: `help/canopen_workbench.md`
- Payload formats: `help/payload_formatter.md`
- Embedded AI and providers: `help/ai_workbench.md`
- Capability coverage: `help/ai_capability_audit.md`

Use Graphify only for source relationships or architecture questions. Live
application state and native capability schemas remain authoritative.
