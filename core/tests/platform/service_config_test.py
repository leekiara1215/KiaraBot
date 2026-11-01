"""Configuration/TLS contract checks; all certificate material lives in a temporary directory."""
import json
from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def certificates(root, key_kind="rsa"):
    def openssl(*arguments):
        subprocess.run(["openssl", *arguments], check=True, cwd=root,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=P02-Test-CA", "-keyout", "ca.key", "-out", "ca.pem")
    key_options = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"] if key_kind == "ec" else ["-newkey", "rsa:2048"]
    openssl("req", *key_options, "-nodes", "-subj", "/CN=localhost",
            "-keyout", "server.key", "-out", "server.csr")
    (root / "extensions").write_text(
        "subjectAltName=DNS:localhost,IP:127.0.0.1\nextendedKeyUsage=serverAuth,clientAuth\n")
    for name, days in [("server.pem", "1")]:
        openssl("x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key",
                "-CAcreateserial", "-days", days, "-extfile", "extensions", "-out", name)
    (root / "index").touch()
    (root / "serial").write_text("1000\n")
    (root / "ca.cnf").write_text(
        "[ca]\ndefault_ca=local\n[local]\ndatabase=index\nserial=serial\n"
        "new_certs_dir=.\ncertificate=ca.pem\nprivate_key=ca.key\ndefault_md=sha256\n"
        "policy=policy\n[policy]\ncommonName=supplied\n")
    openssl("ca", "-batch", "-config", "ca.cnf", "-in", "server.csr",
            "-out", "expired.pem", "-startdate", "200101000000Z", "-enddate", "200102000000Z")
    os.chmod(root / "server.key", 0o600)
    return {
        "schema_version": 1, "service": "identity", "probe_listen": "127.0.0.1:0",
        "grpc_listen": "127.0.0.1:0",
        "tls": {"certificate_file": str(root / "server.pem"),
                "private_key_file": str(root / "server.key"), "ca_file": str(root / "ca.pem")},
    }


def main(driver):
    with tempfile.TemporaryDirectory(prefix="p02-config-") as directory:
        root = Path(directory)
        config = certificates(root)
        path = root / "config.json"

        def expect(expected, value=None, raw=None):
            path.write_text(raw if raw is not None else json.dumps(value or config))
            result = subprocess.run([driver, str(path)], capture_output=True, text=True, timeout=3)
            assert result.returncode == expected, (expected, result.returncode, result.stderr)
            assert "PRIVATE KEY" not in result.stderr and str(root) not in result.stderr

        expect(0)
        path.write_text(json.dumps(config))
        for flag, adjustment, expected in [("-startdate", 0, 0), ("-startdate", -1, 1),
                                           ("-enddate", 0, 1)]:
            encoded = subprocess.check_output(
                ["openssl", "x509", "-in", str(root / "server.pem"), "-noout", flag], text=True)
            instant = datetime.strptime(encoded.strip().split("=", 1)[1], "%b %d %H:%M:%S %Y %Z")
            epoch = int(instant.replace(tzinfo=timezone.utc).timestamp()) + adjustment
            result = subprocess.run([driver, str(path), str(epoch)], capture_output=True, timeout=3)
            assert result.returncode == expected, (flag, adjustment, result.returncode)

        expect(2, {**config, "service": "runtime"})
        expect(2, {**config, "secret": "never-print-this"})
        expect(2, {**config, "probe_listen": "0.0.0.0:1234"})
        expect(2, {**config, "schema_version": 2})
        expect(2, raw=json.dumps(config)[:-1] + ',"service":"identity"}')
        expect(2, {**config, "tls": {**config["tls"], "extra": True}})
        expect(1, {**config, "tls": {**config["tls"], "certificate_file": str(root / "expired.pem")}})
        expect(1, {**config, "tls": {**config["tls"], "private_key_file": str(root / "ca.key")}})
        expect(1, {**config, "tls": {**config["tls"], "ca_file": str(root / "server.pem")}})
        expect(1, {**config, "tls": {**config["tls"], "ca_file": str(root / "missing")}})
        os.chmod(root / "server.key", 0o644)
        expect(1)
        os.chmod(root / "server.key", 0o600)
        (root / "link.key").symlink_to(root / "server.key")
        expect(1, {**config, "tls": {**config["tls"], "private_key_file": str(root / "link.key")}})
        expect(2, raw="{invalid secret-config}")
        os.mkfifo(root / "fifo")
        for field in ["certificate_file", "private_key_file", "ca_file"]:
            expect(1, {**config, "tls": {**config["tls"], field: str(root / "fifo")}})
        fifo_result = subprocess.run([driver, str(root / "fifo")], capture_output=True, timeout=3)
        assert fifo_result.returncode == 2, fifo_result.returncode
        ec_root = root / "ec"
        ec_root.mkdir()
        expect(0, certificates(ec_root, "ec"))
        print("P02 configuration/TLS rejection checks passed")


if __name__ == "__main__":
    main(sys.argv[1])
