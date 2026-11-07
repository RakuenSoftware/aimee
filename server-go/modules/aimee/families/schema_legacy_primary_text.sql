-- Canonical migration consumes retained legacy source through bounded UTF-8
-- chunks. Only this source column supports the existing 16MiB history budget;
-- individual SQL cells and ordinary writer requests remain capped at 1MiB.
ALTER TABLE primary_sessions
 DROP CONSTRAINT IF EXISTS primary_sessions_messages_json_fits_wire;
ALTER TABLE primary_sessions
 ADD CONSTRAINT primary_sessions_messages_json_fits_wire
 CHECK (octet_length(messages_json) <= 16777216);
