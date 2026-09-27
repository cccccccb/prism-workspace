Music's business worker reads `catalog.json` from the Host-provided assets directory.
The UTF-8 JSON schema is version 1, with one to three tracks. Each track requires
`id`, `title`, `artist`, positive `duration_seconds` (at most 7200), and boolean
`favorite`. IDs must be unique ASCII letters/digits/underscore/hyphen; title and
artist are bounded to 96 UTF-8 bytes. Files must be regular, at most 64 KiB, and
remain unchanged while read. Unknown/duplicate fields and invalid values fail
with a visible error and retry action. The package is immutable while running.

This demo prepares real catalogue data asynchronously and drives UI playback
progress. It does not decode tracks or output audio.
