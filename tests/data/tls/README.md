Test-only TLS fixtures for `tests/test_sbi_security.cpp`. **Never use these in a deployment.**

| File | Purpose |
|---|---|
| `ca.pem` | Test CA. Configured as `tls_ca_file`. Its key was discarded after signing. |
| `server.pem` / `server.key` | Server certificate for `127.0.0.1` / `localhost`, signed by `ca.pem` |
| `client.pem` / `client.key` | Client certificate signed by `ca.pem`. Must be accepted. |
| `rogue-client.pem` / `rogue-client.key` | Client certificate signed by an unrelated CA. Must be rejected. |

Every certificate is valid for 100 years, so these fixtures don't expire under CI.
