"""Calibração local do TLS Meter Windows V6 (sem comandos ao ESP32 medidor).

O resultado da regressão é SEMPRE bruto. A aplicação do fator ocorre apenas
na apresentação/exportação, com fator congelado no início de cada ensaio.
Arquivo local: ~/.tls_meter/calibration_windows_v6.json
"""
from __future__ import annotations

import json
import math
import os
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


CALIBRATION_FILE = Path.home() / ".tls_meter" / "calibration_windows_v6.json"


@dataclass
class CalibrationData:
    factor: float = 1.0
    reference_k: float = 0.0
    raw_k: float = 0.0
    sample_id: str = ""
    created_at: str = ""
    enabled: bool = False
    valid: bool = False


def safe_number(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        raise ValueError("Informe um número válido, usando ponto ou vírgula decimal.")
    if not math.isfinite(number):
        raise ValueError("O valor precisa ser finito.")
    return number


def check_reference(k_ref, sample_id):
    value = safe_number(k_ref)
    if value < 0.01 or value > 10.0:
        raise ValueError("A referência deve estar entre 0,01 e 10 W/(m·K).")
    if not sample_id.strip():
        raise ValueError("Identifique a Amostra Padrão e sua ficha técnica.")
    return value


def compute_factor(k_ref, k_raw):
    ref = safe_number(k_ref)
    raw = safe_number(k_raw)
    if ref <= 0 or raw <= 0:
        raise ValueError("A condutividade de referência e a medida devem ser positivas.")
    factor = ref / raw
    if factor < 0.1 or factor > 10:
        raise ValueError("Fator fora da faixa 0,1 a 10: revise ensaio e referência.")
    return factor


def new_calibration(k_ref, k_raw, sample_id):
    ref = check_reference(k_ref, sample_id)
    raw = safe_number(k_raw)
    factor = compute_factor(ref, raw)
    return CalibrationData(
        factor=factor, reference_k=ref, raw_k=raw,
        sample_id=sample_id.strip(), created_at=datetime.now().isoformat(timespec="seconds"),
        enabled=True, valid=True,
    )


def calibrated_values(k_raw, rho_raw, factor):
    f = safe_number(factor)
    if f <= 0:
        raise ValueError("Fator inválido.")
    return k_raw * f if k_raw is not None else None, rho_raw / f if rho_raw is not None else None


def load_calibration(path=None):
    target = Path(path) if path is not None else CALIBRATION_FILE
    try:
        data = json.loads(target.read_text(encoding="utf-8"))
        obj = CalibrationData(**{k: v for k, v in data.items() if k in CalibrationData.__dataclass_fields__})
        if not obj.valid or not math.isfinite(obj.factor) or not 0.1 <= obj.factor <= 10:
            return CalibrationData()
        if not math.isfinite(obj.reference_k) or not math.isfinite(obj.raw_k):
            return CalibrationData()
        return obj
    except (OSError, json.JSONDecodeError, TypeError, ValueError, AttributeError):
        return CalibrationData()


def save_calibration(data: CalibrationData, path=None):
    target = Path(path) if path is not None else CALIBRATION_FILE
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_suffix(target.suffix + ".tmp")
    try:
        temporary.write_text(json.dumps(asdict(data), indent=2, ensure_ascii=False), encoding="utf-8")
        os.replace(str(temporary), str(target))
    finally:
        if temporary.exists():
            temporary.unlink()
