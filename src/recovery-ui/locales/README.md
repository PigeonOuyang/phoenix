# Recovery localisation

`en.inc` and `zh-CN.inc` each contain the same sorted `P4_MSG(identifier, "text")`
records. The identifier is a stable ASCII symbol; the displayed wording can
change without changing that symbol. Use `msg::identifier` at the call site.
When combining text and values, translate the entire template before filling
`{0}`, `{1}`, etc. The compiler checks key order, completeness, and placeholder
agreement. Unknown external text, paths, partition names and hashes are kept.

`log.en.inc` and `log.zh-CN.inc` use the same scheme for installer display text.
`../install_log_locale.hpp` recognizes English producer output and associates it
with a message key. Add an exact rule or a narrowly scoped template there when
adding a translatable installer message. An unrecognized log line stays
verbatim. Translation is applied to the UI's copy, never to the log on disk.

To add a language, copy a catalog and keep its identifiers and placeholders.
Then register the locale and its fonts in the native renderer, preference
validation, and language picker. Only `en` and `zh-CN` are registered today.
Run `bash scripts/test-recovery-ui.sh OUTPUT_DIRECTORY` from the Phoenix tree,
then check the native render gallery in both languages.
