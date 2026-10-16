"""Enable the isolated KV diagnostic only in the inference server environment."""
import os

if os.environ.get("RETARGET_KV_AUDIT_DIR"):
    from kv_audit_probe import install
    install()
