"""
TLS Meter - Windows Application V6.0 / IEEE 442 (Python 3.9)

Esta versão organiza o ensaio em um ciclo explícito:
    1. Estabilização / monitoramento de deriva
    2. Aquecimento
    3. Registro térmico
    4. Resfriamento
    5. Processamento
    6. Conclusão / exportação

Base normativa principal:
- IEEE Std 442-2017 — IEEE Guide for Thermal Resistivity Measurements of
  Soils and Backfill Materials.

IMPORTANTE
---------
A IEEE 442-2017 define geometria, faixas usuais de potência, tempos de
registro, limites térmicos e análise da região linear T x log(t). O critério
numérico automático de deriva usado nesta versão é uma decisão de projeto
configurável e NÃO é apresentado como requisito da IEEE 442.

Bluetooth:
- Bluetooth Classic SPP.
- Protocolo V4:
    PC -> medidor:
        PING
        MONITOR_START
        HEATER_ON,<heat_ms>,<cool_ms>
        HEATER_OFF
        CYCLE_STOP
        BAT?

    medidor -> PC:
        READY,TLS-METER-SPP
        MONITORING
        HEATER_ON
        HEATER_OFF
        CYCLE_END
        CYCLE_STOPPED
        DATA,<PHASE>,<session_ms>,<phase_ms>,<temp_mC>,<power_mW>
        BAT,<battery_mV>,<percent>,<CHARGING|IDLE>

Compatibilidade:
- Linhas antigas elapsed_ms,temp_mC,power_mW ainda são aceitas e atribuídas
  à fase atual, para facilitar desenvolvimento.

Perfis:
1) Laboratório — IEEE 442
   - 10 W/m a 30 W/m
   - registro normativo a cada 15 s durante 10 min de aquecimento
   - interromper se T_sonda - T0 >= 25 °C, sendo T0 obtida automaticamente na estabilização
   - relação L/D de referência: 50:1
   - recipiente cilíndrico: diâmetro interno mínimo de 100 mm

2) Campo — IEEE 442
   - 30 W/m a 50 W/m
   - estabilização prévia de 10 min a 15 min
   - 30 s nos primeiros 5 min; depois 1 min por 30 min a 40 min
   - interromper se T_sonda >= 50 °C

3) Desenvolvimento / simulação
   - ciclo curto para teste de comunicação e interface.

Análise:
- Resistividade térmica rho = 4*pi*a/q
- Condutividade k = 1/rho
- a = inclinação de T versus ln(t)
- q = P/L
- Somente amostras da fase de AQUECIMENTO entram no cálculo TLS.
- A região linear é selecionada automaticamente cobrindo pelo menos
  um ciclo logarítmico, quando houver dados suficientes.
"""

import math
import queue
import threading
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional, List, Tuple, Dict
import tkinter as tk
from tkinter import filedialog, messagebox

import customtkinter as ctk
import numpy as np
import serial
from serial.tools import list_ports
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure
from openpyxl import Workbook
from openpyxl.chart import LineChart, Reference
from openpyxl.styles import Alignment, Font, PatternFill
from calibration_store import (CalibrationData, load_calibration, save_calibration,
                               check_reference, new_calibration, compute_factor,
                               calibrated_values)

APP_VERSION = "6.0-CAL-IEEE442-SPP-PY39"
STANDARD_NAME = "IEEE Std 442-2017"
print(f"TLS Meter {APP_VERSION}")

# -----------------------------------------------------------------------------
# Bluetooth Classic SPP protocol
# -----------------------------------------------------------------------------
DEVICE_NAME = "TLS-METER"
HANDSHAKE_ID = "TLS-METER-SPP"
SERIAL_BAUD = 115200

# -----------------------------------------------------------------------------
# Appearance
# -----------------------------------------------------------------------------
ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("blue")

BG = "#08111F"
PANEL = "#0F1C2E"
PANEL_2 = "#13243A"
PANEL_3 = "#172B45"
TEXT = "#F3F7FC"
MUTED = "#91A4BA"
ACCENT = "#4C8DFF"
SUCCESS = "#37C97D"
DANGER = "#FF5D6C"
WARNING = "#FFBA55"
INFO = "#65B7FF"

PROFILE_LAB = "Laboratório — IEEE 442"
PROFILE_FIELD = "Campo — IEEE 442"
PROFILE_DEV = "Desenvolvimento / simulação"

PROFILES: Dict[str, dict] = {
    PROFILE_LAB: {
        "q_min": 10.0,
        "q_max": 30.0,
        "duration_min_s": 600.0,
        "duration_target_s": 600.0,
        "sample_interval_s": 15.0,
        "delta_temp_abort_c": 25.0,
        "abs_temp_abort_c": None,
        "first5_delta_warn_c": None,
        "ratio_min": 50.0,
        "container_min_mm": 100.0,
        # Estabilização automática: critério de software/projeto, não limiar IEEE.
        "stabilization_min_s": 60.0,
        "drift_window_s": 60.0,
        "drift_limit_c_min": 0.05,
        "cooling_target_s": 300.0,
        "description": "10–30 W/m • aquecimento 10 min • registro 15 s • ciclo TLS completo",
    },
    PROFILE_FIELD: {
        "q_min": 30.0,
        "q_max": 50.0,
        "duration_min_s": 1800.0,
        "duration_target_s": 2400.0,
        "sample_interval_s": None,
        "delta_temp_abort_c": None,
        "abs_temp_abort_c": 50.0,
        "first5_delta_warn_c": 10.0,
        "ratio_min": 40.0,
        "container_min_mm": None,
        # IEEE: permitir 10–15 min para equilíbrio no campo.
        "stabilization_min_s": 600.0,
        "drift_window_s": 120.0,
        "drift_limit_c_min": 0.05,
        "cooling_target_s": 300.0,
        "description": "30–50 W/m • equilíbrio 10–15 min • aquecimento 30–40 min",
    },
    PROFILE_DEV: {
        "q_min": None,
        "q_max": None,
        "duration_min_s": 0.0,
        "duration_target_s": 120.0,
        "sample_interval_s": None,
        "delta_temp_abort_c": None,
        "abs_temp_abort_c": None,
        "first5_delta_warn_c": None,
        "ratio_min": None,
        "container_min_mm": None,
        "stabilization_min_s": 10.0,
        "drift_window_s": 10.0,
        "drift_limit_c_min": 0.30,
        "cooling_target_s": 30.0,
        "description": "Ciclo curto para desenvolvimento: estabilização + 120 s + resfriamento",
    },
}

PHASE_IDLE = "IDLE"
PHASE_STABILIZING = "STABILIZING"
PHASE_HEATING = "HEATING"
PHASE_COOLING = "COOLING"
PHASE_PROCESSING = "PROCESSING"
PHASE_DONE = "DONE"

PHASE_LABELS = {
    PHASE_IDLE: "Aguardando",
    PHASE_STABILIZING: "Estabilização",
    PHASE_HEATING: "Aquecimento",
    PHASE_COOLING: "Resfriamento",
    PHASE_PROCESSING: "Processamento",
    PHASE_DONE: "Concluído",
}



@dataclass
class Sample:
    # time_s = tempo relativo à fase; para cálculo TLS, no aquecimento ele
    # corresponde ao tempo desde a energização do aquecedor.
    time_s: float
    temperature_c: float
    power_w: float
    phase: str = PHASE_HEATING
    session_time_s: float = 0.0


@dataclass
class AnalysisResult:
    slope_ln_k: Optional[float] = None
    intercept_c: Optional[float] = None
    r2: Optional[float] = None
    mean_power_w: Optional[float] = None
    power_stability_pct: Optional[float] = None
    q_w_m: Optional[float] = None
    conductivity_w_mk: Optional[float] = None
    resistivity_km_w: Optional[float] = None
    points_used: int = 0
    recorded_points: int = 0
    fit_start_s: Optional[float] = None
    fit_end_s: Optional[float] = None
    log_span_decades: Optional[float] = None
    delta_t_decade_c: Optional[float] = None
    automatic_window: bool = True
    fit_available: bool = False


class SerialManager:
    """Gerencia a porta COM Bluetooth SPP em uma thread separada."""

    def __init__(self, event_queue: queue.Queue):
        self.event_queue = event_queue
        self.ser: Optional[serial.Serial] = None
        self.reader_thread: Optional[threading.Thread] = None
        self.stop_reader = threading.Event()
        self.port_name: Optional[str] = None
        self._connect_thread: Optional[threading.Thread] = None

    def request_connect(self):
        if self._connect_thread and self._connect_thread.is_alive():
            return
        self._connect_thread = threading.Thread(target=self._connect_worker, daemon=True)
        self._connect_thread.start()

    def request_disconnect(self):
        self._close_serial(notify=True)

    def request_command(self, command: str):
        if not self.ser or not self.ser.is_open:
            self.event_queue.put(("error", "medidor não está conectado."))
            return
        try:
            self.ser.write((command.strip() + "\n").encode("utf-8"))
            self.ser.flush()
        except Exception as exc:
            self.event_queue.put(("error", f"Falha ao enviar comando: {exc}"))
            self._close_serial(notify=True)

    @staticmethod
    def _bluetooth_ports():
        ports = []
        for p in list_ports.comports():
            hay = " ".join([
                str(p.device or ""),
                str(p.description or ""),
                str(p.hwid or ""),
                str(getattr(p, "manufacturer", "") or ""),
            ]).upper()
            if "BTHENUM" in hay or "BLUETOOTH" in hay:
                ports.append(p)
        return ports

    def _connect_worker(self):
        try:
            if self.ser and self.ser.is_open:
                self.event_queue.put(("connected", self.port_name or DEVICE_NAME))
                return

            self.event_queue.put(("status", "Procurando porta COM Bluetooth do TLS-METER..."))
            ports = self._bluetooth_ports()

            if not ports:
                self.event_queue.put((
                    "not_found",
                    "Nenhuma porta COM Bluetooth encontrada. Emparelhe o TLS-METER no Windows primeiro.",
                ))
                return

            for p in ports:
                self.event_queue.put(("status", f"Testando {p.device} — {p.description}"))
                test_ser = None
                try:
                    test_ser = serial.Serial(
                        p.device,
                        baudrate=SERIAL_BAUD,
                        timeout=0.20,
                        write_timeout=1.0,
                    )
                    test_ser.reset_input_buffer()
                    test_ser.reset_output_buffer()

                    import time
                    time.sleep(0.35)
                    test_ser.write(b"PING\n")
                    test_ser.flush()

                    deadline = time.monotonic() + 2.5
                    recognized = False
                    while time.monotonic() < deadline:
                        raw = test_ser.readline()
                        if not raw:
                            continue
                        line = raw.decode("utf-8", errors="replace").strip()
                        if line == f"PONG,{HANDSHAKE_ID}" or line == f"READY,{HANDSHAKE_ID}":
                            recognized = True
                            break

                    if recognized:
                        self.ser = test_ser
                        self.port_name = p.device
                        self.stop_reader.clear()
                        self.reader_thread = threading.Thread(target=self._reader_worker, daemon=True)
                        self.reader_thread.start()
                        self.event_queue.put(("connected", f"{DEVICE_NAME} em {p.device}"))
                        return
                except Exception:
                    pass
                finally:
                    if test_ser is not None and test_ser is not self.ser:
                        try:
                            test_ser.close()
                        except Exception:
                            pass

            self.event_queue.put((
                "not_found",
                "Portas Bluetooth existem, mas nenhuma respondeu como TLS-METER. "
                "Confirme o emparelhamento e a porta SPP no Windows.",
            ))
        except Exception as exc:
            self.event_queue.put(("error", f"Falha na conexão Bluetooth SPP: {exc}"))

    def _reader_worker(self):
        try:
            while not self.stop_reader.is_set():
                if not self.ser or not self.ser.is_open:
                    break
                try:
                    raw = self.ser.readline()
                except Exception as exc:
                    if not self.stop_reader.is_set():
                        self.event_queue.put(("error", f"Conexão Bluetooth serial perdida: {exc}"))
                    break
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").strip()
                if text:
                    self._handle_line(text)
        finally:
            if not self.stop_reader.is_set():
                self._close_serial(notify=True)

    def _handle_line(self, text: str):
        try:
            control_messages = {
                "STARTED", "END", "STOPPED",
                "MONITORING", "HEATER_ON", "HEATER_OFF",
                "CYCLE_END", "CYCLE_STOPPED",
            }
            if text in control_messages:
                self.event_queue.put(("control", text))
                return

            if text.startswith("STATUS,"):
                self.event_queue.put(("device_status", text.split(",", 1)[1]))
                return

            if text.startswith("READY,") or text.startswith("PONG,"):
                return

            # Protocolo de bateria V5:
            # BAT,<battery_mV>,<percent>,<CHARGING|IDLE>
            if text.upper().startswith("BAT,"):
                parts = text.split(",")
                if len(parts) == 4:
                    try:
                        battery_mv = int(parts[1].strip())
                        percent = int(parts[2].strip())
                        charge_state = parts[3].strip().upper()

                        valid = (
                            battery_mv > 0
                            and 0 <= percent <= 100
                            and charge_state in {"CHARGING", "IDLE"}
                        )

                        self.event_queue.put((
                            "battery",
                            {
                                "mv": battery_mv if valid else 0,
                                "percent": percent if valid else -1,
                                "charging": charge_state == "CHARGING" if valid else False,
                                "state": charge_state if valid else "UNKNOWN",
                                "valid": valid,
                            },
                        ))
                    except (ValueError, TypeError):
                        self.event_queue.put((
                            "battery",
                            {
                                "mv": 0,
                                "percent": -1,
                                "charging": False,
                                "state": "UNKNOWN",
                                "valid": False,
                            },
                        ))
                return

            parts = text.split(",")

            # Protocolo V4/V5:
            # DATA,PHASE,session_ms,phase_ms,temp_mC,power_mW
            if len(parts) == 6 and parts[0].upper() == "DATA":
                phase = parts[1].strip().upper()
                session_ms = int(parts[2])
                phase_ms = int(parts[3])
                temp_milli_c = int(parts[4])
                power_milli_w = int(parts[5])
                sample = Sample(
                    time_s=phase_ms / 1000.0,
                    temperature_c=temp_milli_c / 1000.0,
                    power_w=power_milli_w / 1000.0,
                    phase=phase,
                    session_time_s=session_ms / 1000.0,
                )
                self.event_queue.put(("sample", sample))
                return

            # Compatibilidade com protocolo antigo:
            # elapsed_ms,temp_mC,power_mW
            if len(parts) == 3:
                elapsed_ms = int(parts[0])
                temp_milli_c = int(parts[1])
                power_milli_w = int(parts[2])
                sample = Sample(
                    time_s=elapsed_ms / 1000.0,
                    temperature_c=temp_milli_c / 1000.0,
                    power_w=power_milli_w / 1000.0,
                    phase="LEGACY",
                    session_time_s=elapsed_ms / 1000.0,
                )
                self.event_queue.put(("sample", sample))
                return

            self.event_queue.put(("warning", f"Linha serial ignorada: {text}"))
        except Exception as exc:
            self.event_queue.put(("warning", f"Erro ao interpretar pacote serial: {exc}"))

    def _close_serial(self, notify=False):
        self.stop_reader.set()
        ser = self.ser
        self.ser = None
        self.port_name = None
        if ser is not None:
            try:
                if ser.is_open:
                    ser.close()
            except Exception:
                pass
        if notify:
            self.event_queue.put(("disconnected", None))


class MetricCard(ctk.CTkFrame):
    def __init__(self, master, title: str, value: str, unit: str = ""):
        super().__init__(master, fg_color=PANEL, corner_radius=16)
        self.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(
            self, text=title, text_color=MUTED,
            font=ctk.CTkFont(size=11, weight="bold"), anchor="w"
        ).grid(row=0, column=0, sticky="ew", padx=16, pady=(13, 2))
        self.value_label = ctk.CTkLabel(
            self, text=value, text_color=TEXT,
            font=ctk.CTkFont(size=24, weight="bold"), anchor="w"
        )
        self.value_label.grid(row=1, column=0, sticky="ew", padx=16, pady=(0, 2))
        self.unit_label = ctk.CTkLabel(
            self, text=unit, text_color=MUTED,
            font=ctk.CTkFont(size=10), anchor="w"
        )
        self.unit_label.grid(row=2, column=0, sticky="ew", padx=16, pady=(0, 12))

    def set(self, value: str, unit: Optional[str] = None):
        self.value_label.configure(text=value)
        if unit is not None:
            self.unit_label.configure(text=unit)


class CheckBadge(ctk.CTkFrame):
    def __init__(self, master, title: str):
        super().__init__(master, fg_color=PANEL_2, corner_radius=10)
        self.grid_columnconfigure(1, weight=1)
        self.icon = ctk.CTkLabel(self, text="○", text_color=MUTED, width=22,
                                 font=ctk.CTkFont(size=15, weight="bold"))
        self.icon.grid(row=0, column=0, padx=(10, 3), pady=7)
        self.label = ctk.CTkLabel(self, text=title, text_color=MUTED, anchor="w",
                                  font=ctk.CTkFont(size=10))
        self.label.grid(row=0, column=1, sticky="ew", padx=(0, 8), pady=7)

    def set_state(self, state: str, detail: str):
        if state == "ok":
            self.icon.configure(text="✓", text_color=SUCCESS)
            self.label.configure(text=detail, text_color=TEXT)
        elif state == "fail":
            self.icon.configure(text="!", text_color=DANGER)
            self.label.configure(text=detail, text_color=TEXT)
        elif state == "warn":
            self.icon.configure(text="△", text_color=WARNING)
            self.label.configure(text=detail, text_color=TEXT)
        else:
            self.icon.configure(text="○", text_color=MUTED)
            self.label.configure(text=detail, text_color=MUTED)


class TLSApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("TLS Meter V6 | IEEE Std 442-2017")
        self.geometry("1440x860")
        self.minsize(1180, 720)
        self.configure(fg_color=BG)
        self.protocol("WM_DELETE_WINDOW", self.on_close)

        self.events: queue.Queue = queue.Queue()
        self.bt = SerialManager(self.events)
        # self.samples contém somente a fase de aquecimento e é a base
        # dos cálculos IEEE/TLS. all_samples guarda o ciclo inteiro.
        self.samples: List[Sample] = []
        self.all_samples: List[Sample] = []
        self.stabilization_samples: List[Sample] = []
        self.cooling_samples: List[Sample] = []
        self.analysis_samples: List[Sample] = []
        self.fit_samples: List[Sample] = []
        self.analysis = AnalysisResult()
        # Calibracao em arquivo local; nunca encaminhada ao firmware do medidor.
        self.calibration = load_calibration()
        self.calibration_candidate = None
        self.calibration_run = False
        self._calibration_start_requested = False
        self.calibration_run_reference = 0.0
        self.calibration_run_sample_id = ""
        self.run_factor = 1.0
        self.run_calibration_snapshot = CalibrationData()
        self.connected = False
        self.running = False
        self.current_phase = PHASE_IDLE
        self.graph_mode = "Temperatura × tempo"
        self.last_plot_count = -1
        self.abort_reason: Optional[str] = None
        self.warning_reason: Optional[str] = None
        self.baseline_temperature_c: Optional[float] = None
        self.stabilization_drift_c_min: Optional[float] = None
        self.stabilization_ready = False
        self.heater_command_sent = False
        self.cooling_command_sent = False
        self.cycle_started_at: Optional[datetime] = None

        # Telemetria de bateria recebida do ESP32 de medição.
        # Fica inválida sempre que o Bluetooth desconecta e só volta a ser
        # apresentada após um NOVO pacote BAT.
        self.battery_valid = False
        self.battery_mv: Optional[int] = None
        self.battery_percent: Optional[int] = None
        self.battery_charging = False

        self._build_layout()
        self._apply_profile(PROFILE_LAB)
        self._refresh_calibration_tab()
        self.after(100, self._poll_events)
        self.after(250, self._refresh_ui)

    # ------------------------------------------------------------------ UI --
    def _build_layout(self):
        self.grid_columnconfigure(0, weight=0)
        self.grid_columnconfigure(1, weight=1)
        self.grid_rowconfigure(0, weight=1)

        # Left sidebar, scrollable for all test parameters.
        self.sidebar = ctk.CTkScrollableFrame(
            self, width=330, fg_color="#0A1627", corner_radius=0,
            scrollbar_button_color=PANEL_2, scrollbar_button_hover_color=PANEL_3
        )
        self.sidebar.grid(row=0, column=0, sticky="nsew")
        self.sidebar.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            self.sidebar, text="TLS METER", text_color=TEXT,
            font=ctk.CTkFont(size=25, weight="bold"), anchor="w"
        ).grid(row=0, column=0, sticky="ew", padx=18, pady=(22, 0))

        ctk.CTkLabel(
            self.sidebar, text="IEEE Std 442-2017", text_color=INFO,
            font=ctk.CTkFont(size=12, weight="bold"), anchor="w"
        ).grid(row=1, column=0, sticky="ew", padx=18, pady=(0, 2))

        ctk.CTkLabel(
            self.sidebar, text="Thermal Resistivity Measurement", text_color=MUTED,
            font=ctk.CTkFont(size=10), anchor="w"
        ).grid(row=2, column=0, sticky="ew", padx=18, pady=(0, 16))

        self.connection_frame = ctk.CTkFrame(self.sidebar, fg_color=PANEL, corner_radius=14)
        self.connection_frame.grid(row=3, column=0, sticky="ew", padx=12, pady=5)
        self.connection_frame.grid_columnconfigure(1, weight=1)

        self.status_dot = ctk.CTkLabel(self.connection_frame, text="●", text_color=DANGER,
                                       font=ctk.CTkFont(size=18), width=24)
        self.status_dot.grid(row=0, column=0, padx=(12, 4), pady=(12, 0))
        self.status_label = ctk.CTkLabel(self.connection_frame, text="Desconectado", text_color=TEXT,
                                         font=ctk.CTkFont(size=13, weight="bold"), anchor="w")
        self.status_label.grid(row=0, column=1, sticky="ew", padx=(0, 10), pady=(12, 0))
        self.status_detail = ctk.CTkLabel(self.connection_frame, text="Nenhum medidor conectado",
                                          text_color=MUTED, font=ctk.CTkFont(size=10),
                                          anchor="w", wraplength=260)
        self.status_detail.grid(row=1, column=0, columnspan=2, sticky="ew", padx=12, pady=(2, 7))

        # --------------------------------------------------------------
        # Bateria do medidor
        # --------------------------------------------------------------
        # O ícone fica dentro do próprio cartão de conexão para deixar
        # claro que a bateria pertence ao ESP32 de medição remoto.
        self.battery_frame = ctk.CTkFrame(
            self.connection_frame,
            fg_color=PANEL_2,
            corner_radius=9,
            height=42,
        )
        self.battery_frame.grid(
            row=2, column=0, columnspan=2, sticky="ew",
            padx=10, pady=(0, 10)
        )
        self.battery_frame.grid_columnconfigure(1, weight=1)

        self.battery_canvas = tk.Canvas(
            self.battery_frame,
            width=48,
            height=26,
            bg=PANEL_2,
            highlightthickness=0,
            bd=0,
        )
        self.battery_canvas.grid(row=0, column=0, rowspan=2, padx=(10, 7), pady=7)

        self.battery_label = ctk.CTkLabel(
            self.battery_frame,
            text="Bateria --%",
            text_color=TEXT,
            font=ctk.CTkFont(size=11, weight="bold"),
            anchor="w",
        )
        self.battery_label.grid(row=0, column=1, sticky="ew", padx=(0, 8), pady=(6, 0))

        self.battery_state_label = ctk.CTkLabel(
            self.battery_frame,
            text="Aguardando telemetria",
            text_color=MUTED,
            font=ctk.CTkFont(size=9),
            anchor="w",
        )
        self.battery_state_label.grid(row=1, column=1, sticky="ew", padx=(0, 8), pady=(0, 6))

        self._draw_battery_icon()

        self.connect_btn = ctk.CTkButton(self.sidebar, text="Procurar e conectar", command=self.connect_bt,
                                         height=40, corner_radius=10)
        self.connect_btn.grid(row=4, column=0, sticky="ew", padx=12, pady=(7, 3))
        self.disconnect_btn = ctk.CTkButton(
            self.sidebar, text="Desconectar", command=self.disconnect_bt, height=34,
            fg_color=PANEL_2, hover_color="#1A3150", state="disabled"
        )
        self.disconnect_btn.grid(row=5, column=0, sticky="ew", padx=12, pady=(3, 14))

        self._section_title(6, "PERFIL DO ENSAIO")
        profile_frame = ctk.CTkFrame(self.sidebar, fg_color=PANEL, corner_radius=14)
        profile_frame.grid(row=7, column=0, sticky="ew", padx=12)
        profile_frame.grid_columnconfigure(0, weight=1)
        self.profile_menu = ctk.CTkOptionMenu(
            profile_frame, values=[PROFILE_LAB, PROFILE_FIELD, PROFILE_DEV],
            command=self._apply_profile, height=36, fg_color=PANEL_2,
            button_color="#27486D", button_hover_color="#315A86"
        )
        self.profile_menu.set(PROFILE_LAB)
        self.profile_menu.grid(row=0, column=0, sticky="ew", padx=10, pady=(10, 5))
        self.profile_desc = ctk.CTkLabel(profile_frame, text="", text_color=MUTED,
                                         wraplength=270, justify="left", anchor="w",
                                         font=ctk.CTkFont(size=10))
        self.profile_desc.grid(row=1, column=0, sticky="ew", padx=10, pady=(2, 10))

        self._section_title(8, "SONDA E AMOSTRA")
        probe_frame = ctk.CTkFrame(self.sidebar, fg_color=PANEL, corner_radius=14)
        probe_frame.grid(row=9, column=0, sticky="ew", padx=12)
        probe_frame.grid_columnconfigure((0, 1), weight=1)

        self.length_entry = self._labeled_entry(probe_frame, "Comprimento ativo L (mm)", "100", 0, 0)
        self.diameter_entry = self._labeled_entry(probe_frame, "Diâmetro D (mm)", "1.5", 0, 1)
        self.container_entry = self._labeled_entry(probe_frame, "Recipiente Ø interno (mm)", "100", 2, 0)

        # T0 é medida automaticamente durante a estabilização.
        t0_box = ctk.CTkFrame(probe_frame, fg_color="transparent")
        t0_box.grid(row=2, column=1, sticky="ew", padx=6, pady=5)
        ctk.CTkLabel(
            t0_box,
            text="Temperatura de referência T0",
            text_color=MUTED,
            anchor="w",
            font=ctk.CTkFont(size=9),
        ).pack(fill="x", pady=(0, 4))
        self.reference_temp_label = ctk.CTkLabel(
            t0_box,
            text=" • Automática",
            text_color=INFO,
            fg_color=PANEL_2,
            corner_radius=8,
            height=32,
            anchor="w",
            padx=10,
            font=ctk.CTkFont(size=10, weight="bold"),
        )
        self.reference_temp_label.pack(fill="x")

        self.material_entry = self._labeled_entry(probe_frame, "Identificação da amostra", "Solo 01", 4, 0, colspan=2)

        self.equilibrium_var = tk.BooleanVar(value=False)
        self.equilibrium_check = ctk.CTkCheckBox(
            probe_frame, text="Equilíbrio térmico prévio confirmado",
            variable=self.equilibrium_var, command=self._on_equilibrium_toggle,
            text_color=MUTED,
            font=ctk.CTkFont(size=10), checkbox_width=19, checkbox_height=19
        )
        self.equilibrium_check.grid(row=6, column=0, columnspan=2, sticky="w", padx=10, pady=(3, 3))

        self.equilibrium_mode_label = ctk.CTkLabel(
            probe_frame,
            text="No campo: desmarcado = estabilização completa pelo programa.",
            text_color=MUTED, wraplength=270, justify="left", anchor="w",
            font=ctk.CTkFont(size=9)
        )
        self.equilibrium_mode_label.grid(
            row=7, column=0, columnspan=2, sticky="ew", padx=10, pady=(0, 10)
        )

        self._section_title(10, "CICLO TLS")
        cycle_frame = ctk.CTkFrame(self.sidebar, fg_color=PANEL, corner_radius=14)
        cycle_frame.grid(row=11, column=0, sticky="ew", padx=12)
        cycle_frame.grid_columnconfigure((0, 1), weight=1)

        # O ciclo avança automaticamente por padrão.
        # Não há mais opção manual para desativar a progressão entre fases.
        self.stabilization_entry = self._labeled_entry(
            cycle_frame, "Estabilização mínima (s)", "60", 0, 0
        )
        self.cooling_entry = self._labeled_entry(
            cycle_frame, "Resfriamento (s)", "300", 0, 1
        )
        self.drift_window_entry = self._labeled_entry(
            cycle_frame, "Janela da taxa de deriva (s)", "60", 2, 0
        )
        self.drift_limit_entry = self._labeled_entry(
            cycle_frame, "Limite |dT/dt| (°C/min)", "0.05", 2, 1
        )

        ctk.CTkLabel(
            cycle_frame,
            text="O ciclo avança automaticamente após cada condição ser atendida. "
                 "O limite da taxa de deriva (dT/dt) é um critério interno configurável do projeto; "
                 "a IEEE 442 não fixa um valor numérico universal.",
            text_color=MUTED, wraplength=275, justify="left", anchor="w",
            font=ctk.CTkFont(size=9)
        ).grid(row=4, column=0, columnspan=2, sticky="ew", padx=10, pady=(5, 10))

        self._section_title(12, "REGIÃO DE ANÁLISE")
        analysis_frame = ctk.CTkFrame(self.sidebar, fg_color=PANEL, corner_radius=14)
        analysis_frame.grid(row=13, column=0, sticky="ew", padx=12)
        analysis_frame.grid_columnconfigure((0, 1), weight=1)

        self.auto_window_var = tk.BooleanVar(value=True)
        self.auto_window_check = ctk.CTkCheckBox(
            analysis_frame, text="Selecionar janela linear automaticamente",
            variable=self.auto_window_var, command=self._toggle_manual_window,
            text_color=TEXT, font=ctk.CTkFont(size=10), checkbox_width=19, checkbox_height=19
        )
        self.auto_window_check.grid(row=0, column=0, columnspan=2, sticky="w", padx=10, pady=(10, 6))

        self.regression_start_entry = self._labeled_entry(analysis_frame, "Início manual (s)", "15", 1, 0)
        self.regression_end_entry = self._labeled_entry(analysis_frame, "Fim manual (s)", "600", 1, 1)

        ctk.CTkLabel(
            analysis_frame,
            text="O software procura trecho linear com ≥ 1 ciclo log. R² é indicador estatístico; a IEEE não fixa limiar numérico.",
            text_color=MUTED, wraplength=275, justify="left", anchor="w",
            font=ctk.CTkFont(size=9)
        ).grid(row=3, column=0, columnspan=2, sticky="ew", padx=10, pady=(4, 10))

        self._toggle_manual_window()

        self.start_btn = ctk.CTkButton(
            self.sidebar, text="▶  Iniciar ciclo TLS", command=self.start_test,
            height=48, corner_radius=12, fg_color=SUCCESS, hover_color="#2FAB69",
            text_color="#06130D", font=ctk.CTkFont(size=14, weight="bold"), state="disabled"
        )
        self.start_btn.grid(row=14, column=0, sticky="ew", padx=12, pady=(18, 4))
        self.stop_btn = ctk.CTkButton(
            self.sidebar, text="■  Parar ciclo", command=self.stop_test,
            height=38, corner_radius=10, fg_color="#542633", hover_color="#703142", state="disabled"
        )
        self.stop_btn.grid(row=15, column=0, sticky="ew", padx=12, pady=4)
        self.export_btn = ctk.CTkButton(
            self.sidebar, text="Exportar relatório Excel", command=self.export_excel,
            height=38, corner_radius=10, fg_color=PANEL_2, hover_color="#1A3150", state="disabled"
        )
        self.export_btn.grid(row=16, column=0, sticky="ew", padx=12, pady=(4, 20))

        # Main area with tabs.
        self.main = ctk.CTkFrame(self, fg_color=BG, corner_radius=0)
        self.main.grid(row=0, column=1, sticky="nsew")
        self.main.grid_columnconfigure(0, weight=1)
        self.main.grid_rowconfigure(0, weight=1)

        self.main_tabs = ctk.CTkTabview(
            self.main,
            fg_color=BG,
            segmented_button_fg_color=PANEL,
            segmented_button_selected_color="#27486D",
            segmented_button_selected_hover_color="#315A86",
            segmented_button_unselected_color=PANEL_2,
            segmented_button_unselected_hover_color="#1A3150",
            corner_radius=14,
        )
        self.main_tabs.grid(row=0, column=0, sticky="nsew", padx=14, pady=12)
        self.main_tabs.add("Medição")
        self.main_tabs.add("Calibração")
        self.main_tabs.add("Créditos")
        self.main_tabs.set("Medição")

        self.measure_tab = self.main_tabs.tab("Medição")
        self.measure_tab.configure(fg_color=BG)
        self.measure_tab.grid_columnconfigure((0, 1, 2, 3), weight=1)
        self.measure_tab.grid_rowconfigure(4, weight=1)

        title_bar = ctk.CTkFrame(self.measure_tab, fg_color="transparent")
        title_bar.grid(row=0, column=0, columnspan=4, sticky="ew", padx=24, pady=(20, 8))
        title_bar.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(
            title_bar, text="Dados",
            text_color=TEXT, font=ctk.CTkFont(size=23, weight="bold"), anchor="w"
        ).grid(row=0, column=0, sticky="ew")
        self.test_state_label = ctk.CTkLabel(
            title_bar, text="AGUARDANDO", fg_color=PANEL_2, text_color=MUTED,
            corner_radius=10, padx=12, pady=6, font=ctk.CTkFont(size=10, weight="bold")
        )
        self.test_state_label.grid(row=0, column=1, sticky="e")

        # Ciclo TLS — visualização explícita das etapas.
        phase_panel = ctk.CTkFrame(self.measure_tab, fg_color=PANEL, corner_radius=14)
        phase_panel.grid(row=1, column=0, columnspan=4, sticky="ew", padx=24, pady=(2, 6))
        phase_panel.grid_columnconfigure((0, 1, 2, 3), weight=1)

        self.phase_badges = {}
        for idx, (phase_key, label) in enumerate([
            (PHASE_STABILIZING, "1  Estabilização"),
            (PHASE_HEATING, "2  Aquecimento"),
            (PHASE_COOLING, "3  Resfriamento"),
            (PHASE_PROCESSING, "4  Processamento"),
        ]):
            badge = ctk.CTkLabel(
                phase_panel, text=label, text_color=MUTED, fg_color=PANEL_2,
                corner_radius=9, pady=7, font=ctk.CTkFont(size=10, weight="bold")
            )
            badge.grid(row=0, column=idx, sticky="ew", padx=4, pady=8)
            self.phase_badges[phase_key] = badge

        self.phase_detail_label = ctk.CTkLabel(
            phase_panel,
            text="Aguardando início do ciclo.",
            text_color=MUTED,
            font=ctk.CTkFont(size=9)
        )
        self.phase_detail_label.grid(row=1, column=0, columnspan=4, pady=(0, 8))

        # Main result cards. Resistivity is primary according to the guide.
        self.temp_card = MetricCard(self.measure_tab, "TEMPERATURA", "--.--", "°C")
        self.temp_card.grid(row=2, column=0, sticky="nsew", padx=(24, 6), pady=6)
        self.rho_card = MetricCard(self.measure_tab, "RESISTIVIDADE ρ", "--", "K·m/W")
        self.rho_card.grid(row=2, column=1, sticky="nsew", padx=6, pady=6)
        self.k_card = MetricCard(self.measure_tab, "CONDUTIVIDADE k", "--", "W/(m·K)")
        self.k_card.grid(row=2, column=2, sticky="nsew", padx=6, pady=6)
        self.r2_card = MetricCard(self.measure_tab, "AJUSTE LINEAR", "--", "R²")
        self.r2_card.grid(row=2, column=3, sticky="nsew", padx=(6, 24), pady=6)

        # IEEE checks strip.
        checks_panel = ctk.CTkFrame(self.measure_tab, fg_color=PANEL, corner_radius=16)
        checks_panel.grid(row=3, column=0, columnspan=4, sticky="ew", padx=24, pady=(6, 8))
        checks_panel.grid_columnconfigure((0, 1, 2, 3, 4), weight=1)
        self.check_power = CheckBadge(checks_panel, "Potência linear")
        self.check_power.grid(row=0, column=0, sticky="ew", padx=(10, 4), pady=10)
        self.check_stability = CheckBadge(checks_panel, "Estabilidade")
        self.check_stability.grid(row=0, column=1, sticky="ew", padx=4, pady=10)
        self.check_geometry = CheckBadge(checks_panel, "Geometria")
        self.check_geometry.grid(row=0, column=2, sticky="ew", padx=4, pady=10)
        self.check_duration = CheckBadge(checks_panel, "Duração")
        self.check_duration.grid(row=0, column=3, sticky="ew", padx=4, pady=10)
        self.check_linear = CheckBadge(checks_panel, "Região linear")
        self.check_linear.grid(row=0, column=4, sticky="ew", padx=(4, 10), pady=10)

        graph_panel = ctk.CTkFrame(self.measure_tab, fg_color=PANEL, corner_radius=18)
        graph_panel.grid(row=4, column=0, columnspan=4, sticky="nsew", padx=24, pady=(0, 10))
        graph_panel.grid_columnconfigure(0, weight=1)
        graph_panel.grid_rowconfigure(1, weight=1)

        graph_header = ctk.CTkFrame(graph_panel, fg_color="transparent")
        graph_header.grid(row=0, column=0, sticky="ew", padx=16, pady=(12, 0))
        graph_header.grid_columnconfigure(0, weight=1)
        self.graph_title = ctk.CTkLabel(
            graph_header, text="Dados em tempo real", text_color=TEXT,
            font=ctk.CTkFont(size=14, weight="bold")
        )
        self.graph_title.grid(row=0, column=0, sticky="w")
        self.graph_selector = ctk.CTkSegmentedButton(
            graph_header,
            values=["Temperatura × tempo", "Temperatura × log10(t)", "Potência × tempo"],
            command=self.change_graph_mode, width=480
        )
        self.graph_selector.set("Temperatura × tempo")
        self.graph_selector.grid(row=0, column=1, sticky="e")

        self.figure = Figure(figsize=(8, 4), dpi=100, facecolor=PANEL)
        self.ax = self.figure.add_subplot(111)
        self._style_axes()
        self.canvas = FigureCanvasTkAgg(self.figure, master=graph_panel)
        self.canvas.get_tk_widget().configure(bg=PANEL, highlightthickness=0)
        self.canvas.get_tk_widget().grid(row=1, column=0, sticky="nsew", padx=10, pady=(4, 10))

        footer = ctk.CTkFrame(self.measure_tab, fg_color="transparent")
        footer.grid(row=5, column=0, columnspan=4, sticky="ew", padx=24, pady=(0, 14))
        footer.grid_columnconfigure(0, weight=1)
        self.footer_status = ctk.CTkLabel(
            footer, text="Sistema pronto. Conecte o medidor para começar.",
            text_color=MUTED, anchor="w", font=ctk.CTkFont(size=10)
        )
        self.footer_status.grid(row=0, column=0, sticky="ew")
        self.metrics_footer = ctk.CTkLabel(
            footer, text="q: -- W/m  |  P: -- W  |  estabilidade: -- %  |  tempo: 0 s",
            text_color=MUTED, anchor="e", font=ctk.CTkFont(size=10)
        )
        self.metrics_footer.grid(row=0, column=1, sticky="e")

        # Calibracao da Amostra Padrao: fluxo separado dos controles de medicao.
        self._build_calibration_tab()
        # Credits tab.
        self._build_credits_tab()

    def _build_calibration_tab(self):
        tab = self.main_tabs.tab("Calibração")
        tab.configure(fg_color=BG)
        tab.grid_columnconfigure(0, weight=1)
        form = ctk.CTkScrollableFrame(tab, fg_color=PANEL, corner_radius=14)
        form.grid(row=0, column=0, sticky="nsew", padx=30, pady=20)
        tab.grid_rowconfigure(0, weight=1)
        form.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(form, text="Calibração com a Amostra Padrão",
            font=ctk.CTkFont(size=22, weight="bold"), text_color=TEXT, anchor="w"
        ).grid(row=0, column=0, sticky="ew", padx=24, pady=(20, 8))
        ctk.CTkLabel(form,
            text=("A calibração é local a este computador: o medidor continua transmitindo "
                  "temperatura e potência brutas. Informe k de referência conforme a ficha da "
                  "Amostra Padrão; execute o ciclo e confirme explicitamente o fator antes de usar."),
            text_color=MUTED, wraplength=760, justify="left", anchor="w"
        ).grid(row=1, column=0, sticky="ew", padx=24, pady=(0, 12))

        ctk.CTkLabel(form, text="Identificação da Amostra Padrão / ficha técnica", anchor="w",
            text_color=TEXT).grid(row=2, column=0, sticky="ew", padx=24, pady=(4, 2))
        self.cal_sample_entry = ctk.CTkEntry(form, placeholder_text="Ex.: Nylon 6.6 / corpo 01 / ficha 01", height=38)
        self.cal_sample_entry.grid(row=3, column=0, sticky="ew", padx=24)
        self.cal_sample_entry.insert(0, self.calibration.sample_id if self.calibration.valid else "")

        ctk.CTkLabel(form, text="Condutividade de referência k_ref [W/(m·K)]", anchor="w",
            text_color=TEXT).grid(row=4, column=0, sticky="ew", padx=24, pady=(10, 2))
        self.cal_ref_entry = ctk.CTkEntry(form, placeholder_text="Valor medido/certificado da peça (não use valor genérico)", height=38)
        self.cal_ref_entry.grid(row=5, column=0, sticky="ew", padx=24)
        if self.calibration.valid:
            self.cal_ref_entry.insert(0, f"{self.calibration.reference_k:.4f}")

        self.cal_status_label = ctk.CTkLabel(form, text="", text_color=INFO,
            font=ctk.CTkFont(size=13, weight="bold"), justify="left", anchor="w")
        self.cal_status_label.grid(row=6, column=0, sticky="ew", padx=24, pady=(14, 4))
        self.cal_details_label = ctk.CTkLabel(form, text="", text_color=MUTED,
            justify="left", anchor="w", wraplength=740)
        self.cal_details_label.grid(row=7, column=0, sticky="ew", padx=24, pady=(0, 12))

        self.cal_start_btn = ctk.CTkButton(form, text="▶  Executar ensaio de calibração",
            fg_color=SUCCESS, text_color=BG, height=42, command=self._start_calibration_test)
        self.cal_start_btn.grid(row=8, column=0, sticky="ew", padx=24, pady=(6, 6))
        self.cal_candidate_label = ctk.CTkLabel(form, text="Nenhum ensaio de calibração pendente.",
            text_color=MUTED, wraplength=740, justify="left", anchor="w")
        self.cal_candidate_label.grid(row=9, column=0, sticky="ew", padx=24, pady=(4, 8))

        buttons = ctk.CTkFrame(form, fg_color="transparent")
        buttons.grid(row=10, column=0, sticky="ew", padx=24)
        buttons.grid_columnconfigure((0, 1), weight=1)
        self.cal_confirm_btn = ctk.CTkButton(buttons, text="Confirmar fator calculado",
            fg_color=SUCCESS, text_color=BG, command=self._confirm_calibration, state="disabled")
        self.cal_confirm_btn.grid(row=0, column=0, sticky="ew", padx=(0, 8), pady=4)
        self.cal_discard_btn = ctk.CTkButton(buttons, text="Descartar proposta",
            fg_color=PANEL_2, command=self._discard_calibration, state="disabled")
        self.cal_discard_btn.grid(row=0, column=1, sticky="ew", padx=(8, 0), pady=4)

        bottom = ctk.CTkFrame(form, fg_color="transparent")
        bottom.grid(row=11, column=0, sticky="ew", padx=24, pady=(10, 14))
        bottom.grid_columnconfigure((0, 1), weight=1)
        self.cal_toggle_btn = ctk.CTkButton(bottom, text="Ativar / desativar",
            fg_color=PANEL_2, command=self._toggle_calibration)
        self.cal_toggle_btn.grid(row=0, column=0, sticky="ew", padx=(0, 8), pady=4)
        ctk.CTkButton(bottom, text="Apagar fator salvo", fg_color="#592B35",
            command=self._reset_calibration).grid(row=0, column=1, sticky="ew", padx=(8, 0), pady=4)
        ctk.CTkLabel(form,
            text=("F = k_ref / k_bruto; k_corrigido = F × k_bruto; "
                  "ρ_corrigida = ρ_bruta / F. Dados brutos e R² são preservados. "
                  "O ajuste só vale para ensaios iniciados após sua ativação; a IHM "
                  "armazena outro fator independente."),
            text_color=MUTED, wraplength=760, justify="left"
        ).grid(row=12, column=0, sticky="ew", padx=24, pady=(0, 18))

    def _refresh_calibration_tab(self):
        if not hasattr(self, "cal_status_label"):
            return
        c = self.calibration
        if c.valid:
            state = "ATIVO" if c.enabled else "DESATIVADO"
            self.cal_status_label.configure(text=f"Fator registrado: {c.factor:.6f}  |  {state}")
            self.cal_details_label.configure(text=(
                f"Amostra: {c.sample_id}  |  referência: {c.reference_k:.5f} W/(m·K)  |  "
                f"medido bruto: {c.raw_k:.5f} W/(m·K)  |  salvo: {c.created_at}"))
            self.cal_toggle_btn.configure(state="normal", text="Desativar fator" if c.enabled else "Ativar fator")
        else:
            self.cal_status_label.configure(text="Sem fator salvo (F = 1,000000)")
            self.cal_details_label.configure(text="A calibração requer um ensaio com a Amostra Padrão e confirmação manual.")
            self.cal_toggle_btn.configure(state="disabled", text="Ativar / desativar")
        if self.calibration_candidate is not None:
            c2 = self.calibration_candidate
            self.cal_candidate_label.configure(text=(
                f"PROPOSTA PENDENTE  |  k_ref = {c2.reference_k:.5f}  |  "
                f"k_bruto = {c2.raw_k:.5f}  |  F = {c2.factor:.6f}  |  "
                f"R² = {self.analysis.r2 if self.analysis.r2 is not None else '--'}\n"
                "Avalie a estabilidade da potência e a qualidade do ensaio antes de confirmar."),
                text_color=WARNING)
            self.cal_confirm_btn.configure(state="normal" if not self.running else "disabled")
            self.cal_discard_btn.configure(state="normal" if not self.running else "disabled")
        else:
            self.cal_candidate_label.configure(text="Nenhum ensaio de calibração pendente.", text_color=MUTED)
            self.cal_confirm_btn.configure(state="disabled")
            self.cal_discard_btn.configure(state="disabled")
        self.cal_start_btn.configure(state="normal" if self.connected and not self.running else "disabled")

    def _start_calibration_test(self):
        if self.running or not self.connected:
            messagebox.showwarning("Calibração", "Conecte o medidor e aguarde o fim do ciclo atual.")
            return
        if self.profile_menu.get() != PROFILE_LAB:
            messagebox.showwarning("Calibração", "Selecione o perfil Laboratório antes do ensaio da Amostra Padrão.")
            return
        sample_id = self.cal_sample_entry.get().strip()
        try:
            reference_k = check_reference(self.cal_ref_entry.get().strip().replace(",", "."), sample_id)
        except ValueError as exc:
            messagebox.showerror("Referência inválida", str(exc))
            return
        if self.calibration_candidate and not messagebox.askyesno(
            "Descartar proposta anterior?", "Uma proposta de fator ainda não foi confirmada. Substituir pelo novo ensaio?"):
            return
        self.calibration_candidate = None
        self._calibration_start_requested = True
        self.calibration_run_reference = reference_k
        self.calibration_run_sample_id = sample_id
        if self.start_test():
            self.main_tabs.set("Medição")
        else:
            self._calibration_start_requested = False
        self._refresh_calibration_tab()

    def _propose_calibration(self):
        if not self.calibration_run:
            return
        if self.abort_reason or self.current_phase == PHASE_IDLE:
            self.calibration_candidate = None
            return
        k = self.analysis.conductivity_w_mk
        if not self.analysis.fit_available or k is None or k <= 0:
            self.footer_status.configure(text="Calibração sem região linear válida: fator não calculado.", text_color=DANGER)
            self.calibration_candidate = None
            self.main_tabs.set("Calibração")
            self._refresh_calibration_tab()
            return
        try:
            self.calibration_candidate = new_calibration(
                self.calibration_run_reference, k, self.calibration_run_sample_id)
        except ValueError as exc:
            self.footer_status.configure(text=f"Calibração recusada: {exc}", text_color=DANGER)
            self.calibration_candidate = None
        self.main_tabs.set("Calibração")
        self._refresh_calibration_tab()

    def _confirm_calibration(self):
        if self.running or self.calibration_candidate is None:
            return
        c = self.calibration_candidate
        question = (f"Confirmar fator {c.factor:.6f} para a Amostra Padrão '{c.sample_id}'?\n\n"
                    "Este fator será aplicado SOMENTE em novos ciclos Windows.\n"
                    "Medições e exportações anteriores não serão alteradas.")
        if not messagebox.askyesno("Confirmar calibração", question):
            return
        try:
            save_calibration(c)
        except OSError as exc:
            messagebox.showerror("Calibração", f"Não foi possível gravar fator: {exc}")
            return
        self.calibration = c
        self.calibration_candidate = None
        self._refresh_calibration_tab()
        messagebox.showinfo("Calibração", "Fator confirmado e salvo neste computador.")

    def _discard_calibration(self):
        if self.running:
            return
        self.calibration_candidate = None
        self._refresh_calibration_tab()

    def _toggle_calibration(self):
        if self.running or not self.calibration.valid:
            return
        self.calibration.enabled = not self.calibration.enabled
        try:
            save_calibration(self.calibration)
        except OSError as exc:
            self.calibration.enabled = not self.calibration.enabled
            messagebox.showerror("Calibração", str(exc))
        self._refresh_calibration_tab()

    def _reset_calibration(self):
        if self.running:
            return
        if not messagebox.askyesno("Apagar calibração?", "Apagar o fator salvo neste computador? Dados de ensaios anteriores são preservados."):
            return
        try:
            save_calibration(CalibrationData())
        except OSError as exc:
            messagebox.showerror("Calibração", str(exc))
            return
        self.calibration = CalibrationData()
        self.calibration_candidate = None
        self._refresh_calibration_tab()

    def _corrected_result_values(self):
        return calibrated_values(self.analysis.conductivity_w_mk,
                                 self.analysis.resistivity_km_w, self.run_factor)

    def _build_credits_tab(self):
        credits_tab = self.main_tabs.tab("Créditos")
        credits_tab.configure(fg_color=BG)
        credits_tab.grid_columnconfigure(0, weight=1)
        credits_tab.grid_rowconfigure(0, weight=1)

        wrapper = ctk.CTkFrame(credits_tab, fg_color="transparent")
        wrapper.grid(row=0, column=0, sticky="nsew", padx=60, pady=50)
        wrapper.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            wrapper,
            text="TLS METER",
            text_color=TEXT,
            font=ctk.CTkFont(size=34, weight="bold"),
        ).grid(row=0, column=0, pady=(30, 4))

        ctk.CTkLabel(
            wrapper,
            text="Sistema de Medição de Resistividade Térmica",
            text_color=INFO,
            font=ctk.CTkFont(size=16, weight="bold"),
        ).grid(row=1, column=0, pady=(0, 28))

        card = ctk.CTkFrame(wrapper, fg_color=PANEL, corner_radius=18)
        card.grid(row=2, column=0, sticky="ew", padx=80, pady=10)
        card.grid_columnconfigure(0, weight=1)

        ctk.CTkLabel(
            card,
            text="UNIVERSIDADE FEDERAL DE CAMPINA GRANDE",
            text_color=TEXT,
            font=ctk.CTkFont(size=19, weight="bold"),
        ).grid(row=0, column=0, padx=30, pady=(28, 5))

        ctk.CTkLabel(
            card,
            text="UFCG",
            text_color=INFO,
            font=ctk.CTkFont(size=15, weight="bold"),
        ).grid(row=1, column=0, padx=30, pady=(0, 22))

        ctk.CTkLabel(
            card,
            text="Laboratório de Alta Tensão (LAT)",
            text_color=TEXT,
            font=ctk.CTkFont(size=17, weight="bold"),
        ).grid(row=2, column=0, padx=30, pady=(0, 24))

        divider = ctk.CTkFrame(card, height=1, fg_color="#29415D")
        divider.grid(row=3, column=0, sticky="ew", padx=50, pady=(0, 22))

        ctk.CTkLabel(
            card,
            text="Desenvolvido por",
            text_color=MUTED,
            font=ctk.CTkFont(size=11),
        ).grid(row=4, column=0, padx=30, pady=(0, 3))

        ctk.CTkLabel(
            card,
            text="Francisco Olimpio",
            text_color=TEXT,
            font=ctk.CTkFont(size=20, weight="bold"),
        ).grid(row=5, column=0, padx=30, pady=(0, 28))

        ctk.CTkLabel(
            wrapper,
            text="Desenvolvido com base na IEEE Std 442-2017",
            text_color=MUTED,
            font=ctk.CTkFont(size=11),
        ).grid(row=3, column=0, pady=(20, 2))

        ctk.CTkLabel(
            wrapper,
            text="Medição de resistividade térmica de solos e materiais diversos",
            text_color=MUTED,
            font=ctk.CTkFont(size=10),
        ).grid(row=4, column=0, pady=(0, 10))

    def _battery_color(self) -> str:
        if not self.battery_valid or self.battery_percent is None:
            return MUTED
        if self.battery_charging:
            return INFO
        if self.battery_percent <= 15:
            return DANGER
        if self.battery_percent <= 30:
            return WARNING
        return SUCCESS

    def _draw_battery_icon(self):
        """Desenha um ícone simples de bateria no painel de conexão."""
        if not hasattr(self, "battery_canvas"):
            return

        c = self.battery_canvas
        c.delete("all")

        outline = self._battery_color()
        x0, y0, x1, y1 = 4, 5, 39, 21

        # Corpo e terminal.
        c.create_rectangle(x0, y0, x1, y1, outline=outline, width=2)
        c.create_rectangle(40, 10, 44, 16, outline=outline, fill=outline, width=1)

        if self.battery_valid and self.battery_percent is not None:
            pct = max(0, min(100, int(self.battery_percent)))
            inner_w = 31
            fill_w = int(round(inner_w * pct / 100.0))
            if pct > 0:
                fill_w = max(2, fill_w)
                c.create_rectangle(
                    6, 7, 6 + fill_w, 19,
                    outline="",
                    fill=outline,
                )
        else:
            # Pequeno traço central para indicar "sem dado".
            c.create_line(13, 13, 30, 13, fill=MUTED, width=2)

        # Raio durante carregamento.
        if self.battery_valid and self.battery_charging:
            c.create_polygon(
                24, 3,
                18, 13,
                23, 13,
                19, 23,
                31, 10,
                25, 10,
                fill=WARNING,
                outline="",
            )

    def _clear_battery_status(self):
        self.battery_valid = False
        self.battery_mv = None
        self.battery_percent = None
        self.battery_charging = False

        if hasattr(self, "battery_label"):
            self.battery_label.configure(text="Bateria --%", text_color=TEXT)
            self.battery_state_label.configure(
                text="Sem dados do medidor",
                text_color=MUTED,
            )
            self._draw_battery_icon()

    def _update_battery_status(
        self,
        battery_mv: int,
        percent: int,
        charging: bool,
        valid: bool = True,
    ):
        if not valid or battery_mv <= 0 or not (0 <= percent <= 100):
            self._clear_battery_status()
            return

        self.battery_valid = True
        self.battery_mv = int(battery_mv)
        self.battery_percent = int(percent)
        self.battery_charging = bool(charging)

        color = self._battery_color()
        self.battery_label.configure(
            text=f"Bateria {self.battery_percent}%",
            text_color=color,
        )

        voltage_v = self.battery_mv / 1000.0
        if self.battery_charging:
            state_text = f"Carregando  •  {voltage_v:.2f} V"
            state_color = INFO
        else:
            state_text = f"Em bateria  •  {voltage_v:.2f} V"
            state_color = MUTED

        self.battery_state_label.configure(
            text=state_text,
            text_color=state_color,
        )
        self._draw_battery_icon()

    def _section_title(self, row: int, text: str):
        ctk.CTkLabel(
            self.sidebar, text=text, text_color=MUTED,
            font=ctk.CTkFont(size=10, weight="bold"), anchor="w"
        ).grid(row=row, column=0, sticky="ew", padx=16, pady=(10, 5))

    def _labeled_entry(self, master, label: str, default: str, row: int, col: int, colspan: int = 1):
        wrapper = ctk.CTkFrame(master, fg_color="transparent")
        wrapper.grid(row=row, column=col, columnspan=colspan, sticky="ew", padx=8, pady=(6, 0))
        wrapper.grid_columnconfigure(0, weight=1)
        ctk.CTkLabel(wrapper, text=label, text_color=MUTED, anchor="w",
                     font=ctk.CTkFont(size=9)).grid(row=0, column=0, sticky="ew", pady=(0, 2))
        entry = ctk.CTkEntry(wrapper, height=32)
        if default:
            entry.insert(0, default)
        entry.grid(row=1, column=0, sticky="ew")
        return entry

    def _toggle_manual_window(self):
        state = "disabled" if self.auto_window_var.get() else "normal"
        self.regression_start_entry.configure(state=state)
        self.regression_end_entry.configure(state=state)

    def _set_cycle_entry(self, entry, value):
        entry.configure(state="normal")
        entry.delete(0, "end")
        entry.insert(0, str(value))

    def _on_equilibrium_toggle(self):
        """No perfil de campo, evita esperar o período longo duas vezes."""
        if not hasattr(self, "stabilization_entry"):
            return

        if self.profile_menu.get() != PROFILE_FIELD:
            return

        if self.equilibrium_var.get():
            # O operador declara que o período externo de equilíbrio já foi
            # cumprido. O programa ainda mede T0 e confirma estabilidade.
            self._set_cycle_entry(self.stabilization_entry, 60.0)
            self._set_cycle_entry(self.drift_window_entry, 60.0)
            self.equilibrium_mode_label.configure(
                text="Confirmado: verificação rápida de 60 s para medir T0 e conferir dT/dt."
            )
        else:
            # Sem confirmação prévia, o próprio programa executa o período
            # mínimo de 10 min do procedimento de campo e continua aguardando
            # caso a taxa de deriva ainda não satisfaça o critério interno.
            self._set_cycle_entry(self.stabilization_entry, 600.0)
            self._set_cycle_entry(self.drift_window_entry, 120.0)
            self.equilibrium_mode_label.configure(
                text="Não confirmado: o programa fará estabilização completa (mín. 10 min)."
            )

    def _apply_profile(self, profile_name: str):
        profile = PROFILES[profile_name]
        self.profile_desc.configure(text=profile["description"])

        if profile_name == PROFILE_FIELD:
            self.equilibrium_check.configure(state="normal", text_color=TEXT)
        else:
            self.equilibrium_check.configure(state="disabled", text_color=MUTED)
            self.equilibrium_var.set(False)

        # Parâmetros-base do ciclo.
        for entry, value in [
            (self.stabilization_entry, profile["stabilization_min_s"]),
            (self.drift_window_entry, profile["drift_window_s"]),
            (self.drift_limit_entry, profile["drift_limit_c_min"]),
            (self.cooling_entry, profile["cooling_target_s"]),
        ]:
            self._set_cycle_entry(entry, value)

        if profile_name == PROFILE_FIELD:
            # Se o usuário já marcou a confirmação, aplica a verificação curta.
            self._on_equilibrium_toggle()
        else:
            self.equilibrium_mode_label.configure(
                text="Aplicável ao perfil de campo."
            )

        if profile_name == PROFILE_LAB:
            regression_start, regression_end = "15", "600"
        elif profile_name == PROFILE_FIELD:
            regression_start, regression_end = "60", "2400"
        else:
            regression_start, regression_end = "5", "120"

        self.regression_start_entry.configure(state="normal")
        self.regression_start_entry.delete(0, "end")
        self.regression_start_entry.insert(0, regression_start)
        self.regression_end_entry.configure(state="normal")
        self.regression_end_entry.delete(0, "end")
        self.regression_end_entry.insert(0, regression_end)

        self._toggle_manual_window()
        self._calculate_analysis()
        self._update_ieee_checks()
        self._update_phase_ui()
        self._draw_plot(force=True)

    def _style_axes(self):
        self.ax.clear()
        self.ax.set_facecolor(PANEL)
        self.ax.tick_params(colors=MUTED, labelsize=9)
        for spine in self.ax.spines.values():
            spine.set_color("#29415D")
        self.ax.grid(True, alpha=0.12)
        self.ax.set_xlabel("Tempo (s)", color=MUTED)
        self.ax.set_ylabel("Temperatura (°C)", color=MUTED)
        self.figure.tight_layout(pad=2.0)

    # -------------------------------------------------------- BLUETOOTH ----
    def connect_bt(self):
        self.connect_btn.configure(state="disabled", text="Procurando...")
        self.status_detail.configure(text="Procurando porta Bluetooth SPP...")
        self.footer_status.configure(text="Procurando TLS-METER via Bluetooth Classic SPP...")
        self.bt.request_connect()

    def disconnect_bt(self):
        if self.running:
            self.stop_test()
        self._clear_battery_status()
        self.bt.request_disconnect()

    # ------------------------------------------------------------- TEST ----
    def _float_entry(self, entry: ctk.CTkEntry, allow_blank=False) -> Optional[float]:
        text = entry.get().strip().replace(",", ".")
        if allow_blank and text == "":
            return None
        return float(text)

    def _validate_test_parameters(self) -> Tuple[bool, str]:
        try:
            length_mm = self._float_entry(self.length_entry)
            diameter_mm = self._float_entry(self.diameter_entry)

            if length_mm is None or length_mm <= 0 or diameter_mm is None or diameter_mm <= 0:
                return False, "Comprimento e diâmetro da sonda devem ser positivos."

            profile = self.profile_menu.get()

            if profile == PROFILE_LAB:
                container_mm = self._float_entry(self.container_entry)
                if container_mm is None or container_mm <= 0:
                    return False, "Informe o diâmetro interno do recipiente da amostra."

            # Campo:
            # - marcado   -> período externo já cumprido; verificação rápida;
            # - desmarcado -> o programa executa a estabilização completa.
            # Em ambos os casos T0 continua sendo medido pelo próprio sistema.

            stabilization_s = self._float_entry(self.stabilization_entry)
            drift_window_s = self._float_entry(self.drift_window_entry)
            drift_limit = self._float_entry(self.drift_limit_entry)
            cooling_s = self._float_entry(self.cooling_entry)
            if stabilization_s is None or stabilization_s <= 0:
                return False, "O tempo mínimo de estabilização deve ser positivo."
            if drift_window_s is None or drift_window_s <= 0:
                return False, "A janela da taxa de deriva deve ser positiva."
            if drift_limit is None or drift_limit <= 0:
                return False, "O limite da taxa de deriva (dT/dt) deve ser positivo."
            if cooling_s is None or cooling_s < 0:
                return False, "O tempo de resfriamento não pode ser negativo."

            if not self.auto_window_var.get():
                start_s = self._float_entry(self.regression_start_entry)
                end_s = self._float_entry(self.regression_end_entry)
                if start_s is None or end_s is None or start_s <= 0 or end_s <= start_s:
                    return False, "A janela manual de regressão é inválida."

            return True, ""
        except ValueError:
            return False, "Confira os parâmetros numéricos do ensaio."

    def _set_phase(self, phase: str, detail: Optional[str] = None):
        self.current_phase = phase
        self._update_phase_ui(detail)

    def _update_phase_ui(self, detail: Optional[str] = None):
        if not hasattr(self, "phase_badges"):
            return
        order = [PHASE_STABILIZING, PHASE_HEATING, PHASE_COOLING, PHASE_PROCESSING]
        current_index = order.index(self.current_phase) if self.current_phase in order else -1

        for idx, phase in enumerate(order):
            badge = self.phase_badges[phase]
            if self.current_phase == PHASE_DONE:
                badge.configure(fg_color="#173E31", text_color=SUCCESS)
            elif idx < current_index:
                badge.configure(fg_color="#173E31", text_color=SUCCESS)
            elif idx == current_index:
                badge.configure(fg_color="#27486D", text_color=TEXT)
            else:
                badge.configure(fg_color=PANEL_2, text_color=MUTED)

        if detail is None:
            if self.current_phase == PHASE_STABILIZING:
                drift = (
                    f"{self.stabilization_drift_c_min:+.4f} °C/min"
                    if self.stabilization_drift_c_min is not None else "calculando..."
                )
                detail = f"Monitorando equilíbrio térmico • Taxa de deriva (dT/dt): {drift}"
            elif self.current_phase == PHASE_HEATING:
                if self.baseline_temperature_c is not None and self.all_samples:
                    current_t = self.all_samples[-1].temperature_c
                    delta_t = current_t - self.baseline_temperature_c
                    detail = (
                        f"Aquecedor energizado • T0 = {self.baseline_temperature_c:.3f} °C "
                        f"• ΔT = {delta_t:.3f} °C. Somente esta fase entra no cálculo TLS."
                    )
                else:
                    detail = "Aquecedor energizado. Somente esta fase é usada no cálculo TLS."
            elif self.current_phase == PHASE_COOLING:
                detail = "Aquecedor desligado. Temperatura continua sendo registrada."
            elif self.current_phase == PHASE_PROCESSING:
                detail = "Selecionando região linear T × log(t) e calculando ρ e k."
            elif self.current_phase == PHASE_DONE:
                detail = "Ciclo finalizado. Resultados e dados disponíveis para exportação."
            else:
                detail = "Aguardando início do ciclo."
        self.phase_detail_label.configure(text=detail)

    def _stabilization_drift(self) -> Optional[float]:
        if len(self.stabilization_samples) < 3:
            return None
        try:
            window_s = self._float_entry(self.drift_window_entry)
        except ValueError:
            return None
        if window_s is None or window_s <= 0:
            return None

        end_t = self.stabilization_samples[-1].time_s
        window = [s for s in self.stabilization_samples if s.time_s >= end_t - window_s]
        if len(window) < 3:
            return None
        x = np.array([s.time_s for s in window], dtype=float)
        y = np.array([s.temperature_c for s in window], dtype=float)
        if x[-1] - x[0] < max(2.0, window_s * 0.5):
            return None
        slope_s, _ = np.polyfit(x, y, 1)
        return float(slope_s * 60.0)

    def _update_stabilization_state(self):
        self.stabilization_drift_c_min = self._stabilization_drift()
        if not self.stabilization_samples:
            self.stabilization_ready = False
            return

        try:
            min_s = self._float_entry(self.stabilization_entry)
            limit = self._float_entry(self.drift_limit_entry)
        except ValueError:
            self.stabilization_ready = False
            return

        elapsed = self.stabilization_samples[-1].time_s
        drift_ok = (
            self.stabilization_drift_c_min is not None
            and abs(self.stabilization_drift_c_min) <= limit
        )
        self.stabilization_ready = bool(elapsed >= min_s and drift_ok)

        if self.stabilization_ready and not self.heater_command_sent:
            self._begin_heating()
        else:
            drift_text = (
                f"{self.stabilization_drift_c_min:+.4f} °C/min"
                if self.stabilization_drift_c_min is not None else "calculando"
            )
            self._update_phase_ui(
                f"Estabilização: {elapsed:.0f}/{min_s:.0f} s • Taxa de deriva (dT/dt): {drift_text} "
                f"• critério interno ±{limit:.3f} °C/min"
            )

    def _begin_heating(self):
        if not self.connected or self.heater_command_sent:
            return
        profile = PROFILES[self.profile_menu.get()]
        heat_ms = int(round(profile["duration_target_s"] * 1000.0))
        try:
            cooling_s = self._float_entry(self.cooling_entry)
        except ValueError:
            cooling_s = profile["cooling_target_s"]
        cool_ms = int(round(max(0.0, cooling_s) * 1000.0))

        if self.stabilization_samples:
            # T0 é a média das últimas amostras já aprovadas pelo critério de
            # estabilização. O operador não digita temperatura de referência.
            tail = self.stabilization_samples[-min(10, len(self.stabilization_samples)):]
            self.baseline_temperature_c = float(np.mean([s.temperature_c for s in tail]))
            self.reference_temp_label.configure(
                text=f"{self.baseline_temperature_c:.3f} °C",
                text_color=SUCCESS,
            )
        else:
            # Segurança: sem estabilização medida não se inicia aquecimento.
            self.heater_command_sent = False
            self.footer_status.configure(
                text="Não foi possível obter T0: faltam dados de estabilização.",
                text_color=DANGER,
            )
            return

        self.heater_command_sent = True
        self.bt.request_command(f"HEATER_ON,{heat_ms},{cool_ms}")
        self._set_phase(PHASE_HEATING)
        self.test_state_label.configure(text="AQUECENDO", fg_color="#4A3518", text_color=WARNING)
        self.footer_status.configure(
            text="Estabilização concluída. Comando de aquecimento enviado ao medidor.",
            text_color=MUTED
        )

    def start_test(self):
        if not self.connected:
            messagebox.showwarning("Bluetooth", "Conecte o medidor antes de iniciar o ciclo TLS.")
            return False

        if (
            self.battery_valid
            and self.battery_percent is not None
            and self.battery_percent < 10
        ):
            messagebox.showwarning(
                "Bateria baixa",
                "A bateria do medidor está abaixo de 10%. "
                "O firmware não permite iniciar um novo ensaio nesse nível.",
            )
            return False

        valid, error = self._validate_test_parameters()
        if not valid:
            messagebox.showerror("Parâmetros do ensaio", error)
            return False

        # Uma proposta pendente pertence somente ao ensaio que a gerou.
        # Iniciar outro ciclo a descarta para evitar confirmações sobre dados trocados.
        self.calibration_candidate = None
        # Congela o fator usado neste ciclo. A referencia e o fator novos nao
        # podem alterar retroativamente os resultados de um ciclo anterior.
        self.calibration_run = bool(self._calibration_start_requested)
        self._calibration_start_requested = False
        self.run_calibration_snapshot = CalibrationData(**vars(self.calibration))
        self.run_factor = (self.calibration.factor if self.calibration.valid
                           and self.calibration.enabled and not self.calibration_run else 1.0)
        self.samples.clear()
        self.all_samples.clear()
        self.stabilization_samples.clear()
        self.cooling_samples.clear()
        self.analysis_samples.clear()
        self.fit_samples.clear()
        self.analysis = AnalysisResult()
        self.last_plot_count = -1
        self.abort_reason = None
        self.warning_reason = None
        self.baseline_temperature_c = None
        self.reference_temp_label.configure(
            text=" • Automática",
            text_color=INFO,
        )
        self.stabilization_drift_c_min = None
        self.stabilization_ready = False
        self.heater_command_sent = False
        self.cooling_command_sent = False
        self.cycle_started_at = datetime.now()

        self.running = True
        self._set_phase(PHASE_STABILIZING)
        self._update_controls()
        self.test_state_label.configure(text="ESTABILIZANDO", fg_color="#173149", text_color=INFO)
        if self.profile_menu.get() == PROFILE_FIELD and self.equilibrium_var.get():
            start_msg = (
                "Equilíbrio prévio confirmado: iniciando verificação rápida para medir T0 "
                "e conferir a taxa de deriva (dT/dt)."
            )
        elif self.profile_menu.get() == PROFILE_FIELD:
            start_msg = (
                "Equilíbrio prévio não confirmado: iniciando estabilização completa de campo."
            )
        else:
            start_msg = "Iniciando monitoramento de estabilização térmica..."

        self.footer_status.configure(text=start_msg, text_color=MUTED)
        self.bt.request_command("MONITOR_START")
        self._draw_plot(force=True)
        self._refresh_calibration_tab()
        return True

    def stop_test(self, automatic_reason: Optional[str] = None):
        if self.connected:
            self.bt.request_command("CYCLE_STOP")
        self.running = False
        self.calibration_candidate = None if self.calibration_run else self.calibration_candidate
        if automatic_reason:
            self.abort_reason = automatic_reason
            self.test_state_label.configure(text="ABORTADO", fg_color="#47232C", text_color=DANGER)
            self.footer_status.configure(text=automatic_reason, text_color=DANGER)
        else:
            self.test_state_label.configure(text="INTERROMPIDO", fg_color="#47232C", text_color=DANGER)
            self.footer_status.configure(text="Ciclo interrompido pelo usuário.", text_color=MUTED)
        self._set_phase(PHASE_IDLE, "Ciclo interrompido.")
        self._update_controls()
        self._update_ieee_checks()
        self._refresh_calibration_tab()

    def _safety_check_sample(self, sample: Sample):
        if not self.running or self.current_phase != PHASE_HEATING:
            return

        profile_name = self.profile_menu.get()
        profile = PROFILES[profile_name]

        # A referência térmica é exclusivamente T0 medida na estabilização.
        reference_t0 = self.baseline_temperature_c

        if reference_t0 is not None and profile["delta_temp_abort_c"] is not None:
            delta = sample.temperature_c - reference_t0
            if delta >= profile["delta_temp_abort_c"]:
                self.stop_test(
                    f"Ciclo abortado: ΔT = {delta:.2f} °C atingiu o limite de "
                    f"{profile['delta_temp_abort_c']:.0f} °C do perfil de laboratório."
                )
                return

        if profile["abs_temp_abort_c"] is not None and sample.temperature_c >= profile["abs_temp_abort_c"]:
            self.stop_test(
                f"Ciclo abortado: temperatura da sonda atingiu {sample.temperature_c:.2f} °C "
                f"(limite de {profile['abs_temp_abort_c']:.0f} °C no perfil de campo)."
            )
            return

        if (
            reference_t0 is not None and profile["first5_delta_warn_c"] is not None
            and sample.time_s <= 300.0
        ):
            delta = sample.temperature_c - reference_t0
            if delta > profile["first5_delta_warn_c"]:
                self.warning_reason = (
                    f"Atenção: nos primeiros 5 min de aquecimento, ΔT = {delta:.2f} °C excedeu "
                    f"{profile['first5_delta_warn_c']:.0f} °C."
                )

    # ---------------------------------------------------------- ANALYSIS -----
    @staticmethod
    def _nearest_samples_to_targets(samples: List[Sample], targets: List[float], tolerance: float) -> List[Sample]:
        if not samples:
            return []
        result: List[Sample] = []
        times = np.array([s.time_s for s in samples], dtype=float)
        used_indices = set()
        for target in targets:
            idx = int(np.argmin(np.abs(times - target)))
            if idx in used_indices:
                continue
            if abs(times[idx] - target) <= tolerance:
                result.append(samples[idx])
                used_indices.add(idx)
        return result

    def _protocol_recorded_samples(self) -> List[Sample]:
        if not self.samples:
            return []
        profile = self.profile_menu.get()
        last_t = self.samples[-1].time_s

        if profile == PROFILE_LAB:
            # IEEE 442: temperature data recorded at 15 s intervals for 10 min.
            max_t = min(last_t, 600.0)
            targets = list(np.arange(15.0, max_t + 0.01, 15.0))
            return self._nearest_samples_to_targets(self.samples, targets, tolerance=7.5)

        if profile == PROFILE_FIELD:
            # First 5 min every 30 s; thereafter every 1 min.
            targets: List[float] = []
            t = 30.0
            while t <= min(last_t, 300.0) + 1e-9:
                targets.append(t)
                t += 30.0
            t = 360.0
            while t <= last_t + 1e-9:
                targets.append(t)
                t += 60.0
            return self._nearest_samples_to_targets(self.samples, targets, tolerance=15.0)

        return list(self.samples)

    @staticmethod
    def _fit_window(samples: List[Sample]) -> Optional[Tuple[float, float, float, float, float]]:
        if len(samples) < 4:
            return None
        x = np.log(np.array([s.time_s for s in samples], dtype=float))
        y = np.array([s.temperature_c for s in samples], dtype=float)
        slope, intercept = np.polyfit(x, y, 1)
        y_pred = slope * x + intercept
        ss_res = float(np.sum((y - y_pred) ** 2))
        ss_tot = float(np.sum((y - np.mean(y)) ** 2))
        r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 1.0
        span_decades = math.log10(samples[-1].time_s / samples[0].time_s)
        return float(slope), float(intercept), float(r2), float(span_decades), float(slope * math.log(10.0))

    def _automatic_linear_window(self, recorded: List[Sample]) -> List[Sample]:
        # The guide requires using the straight-line section and describes a
        # convenient one-log-cycle calculation. The exact automated selection
        # algorithm below is a software design decision, not an IEEE criterion.
        if len(recorded) < 6:
            return []

        best_window: List[Sample] = []
        best_score = -1e9
        n = len(recorded)
        for i in range(0, n - 5):
            if recorded[i].time_s <= 0:
                continue
            for j in range(i + 5, n):
                if recorded[j].time_s <= recorded[i].time_s:
                    continue
                span = math.log10(recorded[j].time_s / recorded[i].time_s)
                if span < 1.0:
                    continue
                window = recorded[i:j + 1]
                fit = self._fit_window(window)
                if fit is None:
                    continue
                slope, _intercept, r2, span_decades, _delta_decade = fit
                if slope <= 0:
                    continue
                # Prefer very linear windows, then longer log span and more points.
                score = r2 + 0.0005 * min(span_decades, 2.0) + 0.00001 * len(window)
                if score > best_score:
                    best_score = score
                    best_window = window
        return best_window

    def _manual_linear_window(self, recorded: List[Sample]) -> List[Sample]:
        try:
            start_s = self._float_entry(self.regression_start_entry)
            end_s = self._float_entry(self.regression_end_entry)
        except ValueError:
            return []
        if start_s is None or end_s is None:
            return []
        return [s for s in recorded if start_s <= s.time_s <= end_s and s.time_s > 0]

    def _calculate_analysis(self):
        recorded = self._protocol_recorded_samples()
        self.analysis_samples = recorded

        if len(self.samples) < 2:
            self.analysis = AnalysisResult(recorded_points=len(recorded))
            self.fit_samples = []
            return

        try:
            length_m = self._float_entry(self.length_entry) / 1000.0
        except (ValueError, TypeError):
            return
        if length_m <= 0:
            return

        powers_all = np.array([s.power_w for s in self.samples if s.power_w > 0], dtype=float)
        mean_power = float(np.mean(powers_all)) if len(powers_all) else None
        stability = None
        q_w_m = None
        if mean_power is not None and mean_power > 0:
            stability = float(np.max(np.abs(powers_all - mean_power)) / mean_power * 100.0)
            q_w_m = mean_power / length_m

        if self.auto_window_var.get():
            fit_samples = self._automatic_linear_window(recorded)
        else:
            fit_samples = self._manual_linear_window(recorded)
        self.fit_samples = fit_samples

        if len(fit_samples) < 4:
            self.analysis = AnalysisResult(
                mean_power_w=mean_power,
                power_stability_pct=stability,
                q_w_m=q_w_m,
                points_used=len(fit_samples),
                recorded_points=len(recorded),
                automatic_window=self.auto_window_var.get(),
                fit_available=False,
            )
            return

        fit = self._fit_window(fit_samples)
        if fit is None:
            return
        slope, intercept, r2, span_decades, delta_decade = fit

        rho = None
        conductivity = None
        if slope > 0 and q_w_m is not None and q_w_m > 0:
            # IEEE 442 Eq. (1) with slope a = ΔT / Δln(t): rho = 4*pi*a/q
            rho = 4.0 * math.pi * slope / q_w_m
            if rho > 0:
                conductivity = 1.0 / rho

        self.analysis = AnalysisResult(
            slope_ln_k=slope,
            intercept_c=intercept,
            r2=r2,
            mean_power_w=mean_power,
            power_stability_pct=stability,
            q_w_m=q_w_m,
            conductivity_w_mk=conductivity,
            resistivity_km_w=rho,
            points_used=len(fit_samples),
            recorded_points=len(recorded),
            fit_start_s=fit_samples[0].time_s,
            fit_end_s=fit_samples[-1].time_s,
            log_span_decades=span_decades,
            delta_t_decade_c=delta_decade,
            automatic_window=self.auto_window_var.get(),
            fit_available=True,
        )

    # --------------------------------------------------------- IEEE CHECKS --
    def _check_states(self) -> dict:
        profile_name = self.profile_menu.get()
        profile = PROFILES[profile_name]
        states = {}

        # q range
        q = self.analysis.q_w_m
        if profile["q_min"] is None:
            states["power"] = ("pending", f"q = {q:.1f} W/m" if q is not None else "q aguardando dados")
        elif q is None:
            states["power"] = ("pending", f"q alvo {profile['q_min']:.0f}–{profile['q_max']:.0f} W/m")
        elif profile["q_min"] <= q <= profile["q_max"]:
            states["power"] = ("ok", f"q {q:.1f} W/m")
        else:
            states["power"] = ("fail", f"q {q:.1f} W/m fora de {profile['q_min']:.0f}–{profile['q_max']:.0f}")

        # ±1% heat flux / power stability
        st = self.analysis.power_stability_pct
        if st is None:
            states["stability"] = ("pending", "estabilidade aguardando")
        elif st <= 1.0:
            states["stability"] = ("ok", f"±{st:.2f}% ≤ 1%")
        else:
            states["stability"] = ("fail", f"±{st:.2f}% > 1%")

        # Geometry
        try:
            length_mm = self._float_entry(self.length_entry)
            diameter_mm = self._float_entry(self.diameter_entry)
            ratio = length_mm / diameter_mm
        except Exception:
            ratio = None
        ratio_min = profile["ratio_min"]
        if ratio_min is None:
            states["geometry"] = ("pending", f"L/D {ratio:.1f}" if ratio else "geometria livre")
        elif ratio is None:
            states["geometry"] = ("pending", f"L/D alvo ≥ {ratio_min:.0f}")
        elif ratio >= ratio_min:
            detail = f"L/D {ratio:.1f}"
            if profile_name == PROFILE_LAB:
                try:
                    container_mm = self._float_entry(self.container_entry)
                    if container_mm is not None and container_mm < 100.0:
                        states["geometry"] = ("fail", f"L/D {ratio:.1f}; recipiente <100 mm")
                    else:
                        states["geometry"] = ("ok", detail)
                except Exception:
                    states["geometry"] = ("warn", f"L/D {ratio:.1f}; confira recipiente")
            else:
                states["geometry"] = ("ok", detail)
        else:
            states["geometry"] = ("fail", f"L/D {ratio:.1f} < {ratio_min:.0f}")

        # Duration
        t = self.samples[-1].time_s if self.samples else 0.0
        min_duration = profile["duration_min_s"]
        if min_duration <= 0:
            states["duration"] = ("pending", f"{t:.0f} s")
        elif t >= min_duration:
            states["duration"] = ("ok", f"{t/60.0:.1f} min")
        else:
            states["duration"] = ("pending" if self.running else "fail",
                                  f"{t/60.0:.1f}/{min_duration/60.0:.0f} min")

        # Linear section with one log cycle. R² shown but not treated as an IEEE pass/fail threshold.
        if not self.analysis.fit_available:
            states["linear"] = ("pending", "aguardando ≥1 ciclo log")
        elif self.analysis.log_span_decades is not None and self.analysis.log_span_decades >= 1.0:
            states["linear"] = ("ok", f"{self.analysis.log_span_decades:.2f} ciclos; R² {self.analysis.r2:.4f}")
        else:
            states["linear"] = ("warn", f"janela <1 ciclo log; R² {self.analysis.r2:.4f}")

        return states

    def _update_ieee_checks(self):
        states = self._check_states()
        self.check_power.set_state(*states["power"])
        self.check_stability.set_state(*states["stability"])
        self.check_geometry.set_state(*states["geometry"])
        self.check_duration.set_state(*states["duration"])
        self.check_linear.set_state(*states["linear"])

    def _final_software_assessment(self) -> str:
        profile = self.profile_menu.get()
        if profile == PROFILE_DEV:
            return "Modo de desenvolvimento — avaliação normativa desativada"
        states = self._check_states()
        failures = [k for k, v in states.items() if v[0] == "fail"]
        pending = [k for k, v in states.items() if v[0] == "pending"]
        if self.abort_reason:
            return "NÃO ATENDE — ensaio abortado por limite térmico"
        if failures:
            return "NÃO ATENDE às verificações de software IEEE 442"
        if pending:
            return "INCOMPLETO — faltam condições/dados para avaliação"
        return "ATENDE às verificações implementadas no software (não implica certificação do sistema)"

    # --------------------------------------------------------------- PLOT ----
    def change_graph_mode(self, value: str):
        self.graph_mode = value
        self._draw_plot(force=True)

    def _draw_plot(self, force=False):
        count = len(self.all_samples) + len(self.samples)
        if not force and self.last_plot_count == count:
            return
        self.last_plot_count = count

        self.ax.clear()
        self.ax.set_facecolor(PANEL)
        self.ax.tick_params(colors=MUTED, labelsize=9)
        for spine in self.ax.spines.values():
            spine.set_color("#29415D")
        self.ax.grid(True, alpha=0.12)

        source_all = self.all_samples if self.all_samples else self.samples
        if not source_all:
            self.ax.set_xlabel("Tempo (s)", color=MUTED)
            self.ax.set_ylabel("Temperatura (°C)", color=MUTED)
            self.ax.text(
                0.5, 0.5, "Os dados aparecerão aqui quando o ciclo começar",
                color=MUTED, ha="center", va="center", transform=self.ax.transAxes
            )
            self.figure.tight_layout(pad=2.0)
            self.canvas.draw_idle()
            return

        if self.graph_mode == "Temperatura × tempo":
            # Ciclo completo em tempo de sessão.
            for phase, label in [
                (PHASE_STABILIZING, "Estabilização"),
                (PHASE_HEATING, "Aquecimento"),
                (PHASE_COOLING, "Resfriamento"),
            ]:
                vals = [s for s in source_all if s.phase == phase]
                if vals:
                    self.ax.plot(
                        [s.session_time_s for s in vals],
                        [s.temperature_c for s in vals],
                        linewidth=1.7, label=label
                    )
            # Compatibilidade com dados legados.
            legacy = [s for s in source_all if s.phase not in {
                PHASE_STABILIZING, PHASE_HEATING, PHASE_COOLING
            }]
            if legacy:
                self.ax.plot(
                    [s.session_time_s for s in legacy],
                    [s.temperature_c for s in legacy],
                    linewidth=1.7, label="Dados"
                )
            if self.baseline_temperature_c is not None:
                self.ax.axhline(
                    self.baseline_temperature_c, linestyle="--", linewidth=1.0,
                    alpha=0.75, label="Linha de base"
                )
            self.ax.set_xlabel("Tempo do ciclo (s)", color=MUTED)
            self.ax.set_ylabel("Temperatura (°C)", color=MUTED)
            self.ax.legend(facecolor=PANEL, edgecolor="#29415D", labelcolor=TEXT, fontsize=8)

        elif self.graph_mode == "Potência × tempo":
            heating = self.samples
            if heating:
                self.ax.plot(
                    [s.time_s for s in heating],
                    [s.power_w for s in heating],
                    linewidth=1.8, label="Potência — aquecimento"
                )
                if self.analysis.mean_power_w is not None:
                    self.ax.axhline(
                        self.analysis.mean_power_w, linestyle="--", linewidth=1.2,
                        label="Potência média"
                    )
                    self.ax.legend(facecolor=PANEL, edgecolor="#29415D", labelcolor=TEXT, fontsize=8)
            self.ax.set_xlabel("Tempo desde HEATER ON (s)", color=MUTED)
            self.ax.set_ylabel("Potência (W)", color=MUTED)

        else:  # Temperatura × log10(t)
            heating = [s for s in self.samples if s.time_s > 0]
            if heating:
                x = np.log10(np.array([s.time_s for s in heating], dtype=float))
                y = np.array([s.temperature_c for s in heating], dtype=float)
                self.ax.plot(x, y, linewidth=1.5, alpha=0.65, label="Aquecimento — dados brutos")

            if self.analysis_samples:
                xs = [math.log10(s.time_s) for s in self.analysis_samples if s.time_s > 0]
                ys = [s.temperature_c for s in self.analysis_samples if s.time_s > 0]
                if xs:
                    self.ax.scatter(xs, ys, s=18, alpha=0.8, label="Pontos do protocolo")

            if self.fit_samples and self.analysis.fit_available:
                xs = np.array([math.log10(s.time_s) for s in self.fit_samples], dtype=float)
                ys = np.array([s.temperature_c for s in self.fit_samples], dtype=float)
                self.ax.scatter(xs, ys, s=28, label="Usados na regressão")
                xfit = np.linspace(xs.min(), xs.max(), 100)
                # T = a*ln(t)+b = a*ln(10)*log10(t)+b
                yfit = self.analysis.slope_ln_k * math.log(10.0) * xfit + self.analysis.intercept_c
                self.ax.plot(xfit, yfit, linestyle="--", linewidth=1.4, label="Ajuste linear")

            self.ax.set_xlabel("log10(t), t em s desde HEATER ON", color=MUTED)
            self.ax.set_ylabel("Temperatura (°C)", color=MUTED)
            self.ax.legend(facecolor=PANEL, edgecolor="#29415D", labelcolor=TEXT, fontsize=8)

        self.figure.tight_layout(pad=2.0)
        self.canvas.draw_idle()

    # ------------------------------------------------------------- EVENTS ----
    def _poll_events(self):
        while True:
            try:
                event, payload = self.events.get_nowait()
            except queue.Empty:
                break

            if event == "connected":
                self.connected = True
                self._clear_battery_status()
                self.status_dot.configure(text_color=SUCCESS)
                self.status_label.configure(text="Conectado")
                self.status_detail.configure(text=f"{payload} disponível para ensaio")
                self.footer_status.configure(
                    text="Medidor conectado. Sistema pronto para iniciar o ciclo TLS.",
                    text_color=MUTED
                )
                self.connect_btn.configure(text="Conectado", state="disabled")
                self.disconnect_btn.configure(state="normal")
                self._update_controls()

                # Solicita um pacote novo imediatamente. O firmware real
                # também envia BAT periodicamente a cada ~10 s.
                self.bt.request_command("BAT?")

            elif event == "disconnected":
                was_running = self.running
                self.connected = False
                self.running = False
                self._clear_battery_status()
                self.status_dot.configure(text_color=DANGER)
                self.status_label.configure(text="Desconectado")
                self.status_detail.configure(text="Nenhum medidor conectado")

                if was_running:
                    self.abort_reason = (
                        "Ensaio abortado: conexão Bluetooth perdida. "
                        "O firmware V4.2 desliga imediatamente o aquecedor."
                    )
                    self.test_state_label.configure(
                        text="ABORTADO", fg_color="#47232C", text_color=DANGER
                    )
                    self.footer_status.configure(text=self.abort_reason, text_color=DANGER)
                    self._set_phase(PHASE_IDLE, "Ensaio abortado por perda de Bluetooth.")
                else:
                    self.footer_status.configure(
                        text="Conexão Bluetooth encerrada.",
                        text_color=DANGER
                    )
                    self.test_state_label.configure(
                        text="SEM CONEXÃO", fg_color="#47232C", text_color=DANGER
                    )

                self.connect_btn.configure(text="Procurar e conectar", state="normal")
                self.disconnect_btn.configure(state="disabled")
                self._update_controls()

            elif event == "not_found":
                self.connected = False
                self._clear_battery_status()
                self.status_dot.configure(text_color=DANGER)
                self.status_label.configure(text="Não encontrado")
                detail = str(payload) if payload else "TLS-METER não foi localizado"
                self.status_detail.configure(text=detail)
                self.footer_status.configure(text=detail, text_color=DANGER)
                self.connect_btn.configure(text="Tentar novamente", state="normal")

            elif event == "status":
                self.status_detail.configure(text=str(payload))
                self.footer_status.configure(text=str(payload), text_color=MUTED)

            elif event == "sample":
                sample: Sample = payload

                # Compatibilidade com firmware antigo: atribui a fase atual.
                if sample.phase == "LEGACY":
                    sample.phase = self.current_phase if self.current_phase != PHASE_IDLE else PHASE_HEATING

                self.all_samples.append(sample)

                if sample.phase == PHASE_STABILIZING:
                    self.stabilization_samples.append(sample)
                    if self.current_phase != PHASE_STABILIZING:
                        self._set_phase(PHASE_STABILIZING)
                    self._update_stabilization_state()

                elif sample.phase == PHASE_HEATING:
                    self.samples.append(sample)
                    if self.current_phase != PHASE_HEATING:
                        self._set_phase(PHASE_HEATING)
                    self._safety_check_sample(sample)
                    self._calculate_analysis()
                    self._update_ieee_checks()

                elif sample.phase == PHASE_COOLING:
                    self.cooling_samples.append(sample)
                    if self.current_phase != PHASE_COOLING:
                        self._set_phase(PHASE_COOLING)
                        self.test_state_label.configure(
                            text="RESFRIANDO", fg_color="#173149", text_color=INFO
                        )

                self.export_btn.configure(state="normal")

            elif event == "battery":
                data = payload if isinstance(payload, dict) else {}
                self._update_battery_status(
                    battery_mv=int(data.get("mv", 0) or 0),
                    percent=int(data.get("percent", -1) or -1),
                    charging=bool(data.get("charging", False)),
                    valid=bool(data.get("valid", False)),
                )
                self._update_controls()

            elif event == "control":
                if payload == "MONITORING":
                    self._set_phase(PHASE_STABILIZING)
                    self.footer_status.configure(
                        text="Monitoramento iniciado. Aguardando estabilização térmica...",
                        text_color=MUTED
                    )

                elif payload in {"STARTED", "HEATER_ON"}:
                    self.running = True
                    self.heater_command_sent = True
                    self._set_phase(PHASE_HEATING)
                    self.test_state_label.configure(
                        text="AQUECENDO", fg_color="#4A3518", text_color=WARNING
                    )
                    self.footer_status.configure(
                        text="Aquecimento iniciado. Registrando resposta térmica...",
                        text_color=MUTED
                    )

                elif payload == "HEATER_OFF":
                    self.cooling_command_sent = True
                    self._set_phase(PHASE_COOLING)
                    self.test_state_label.configure(
                        text="RESFRIANDO", fg_color="#173149", text_color=INFO
                    )
                    self.footer_status.configure(
                        text="Aquecedor desligado. Registrando curva de resfriamento...",
                        text_color=MUTED
                    )

                elif payload in {"END", "CYCLE_END"}:
                    self.running = False
                    self._set_phase(PHASE_PROCESSING)
                    self.test_state_label.configure(
                        text="PROCESSANDO", fg_color="#263A55", text_color=INFO
                    )
                    self._calculate_analysis()
                    self._update_ieee_checks()
                    assessment = self._final_software_assessment()

                    self._set_phase(PHASE_DONE)
                    # Ciclo concluído normalmente = estado operacional verde.
                    # Eventuais avisos de qualidade/validação permanecem no
                    # diagnóstico e não alteram a cor do estado "CONCLUÍDO".
                    self.test_state_label.configure(
                        text="CONCLUÍDO", fg_color="#173E31", text_color=SUCCESS
                    )
                    self.footer_status.configure(text=assessment, text_color=TEXT)
                    self._update_controls()
                    self._propose_calibration()

                elif payload in {"STOPPED", "CYCLE_STOPPED"}:
                    self.running = False
                    if not self.abort_reason:
                        self.test_state_label.configure(
                            text="INTERROMPIDO", fg_color="#47232C", text_color=DANGER
                        )
                    self._update_controls()

            elif event == "device_status":
                if str(payload) == "START_BLOCKED_LOW_BATTERY":
                    self.footer_status.configure(
                        text=(
                            "Novo ensaio bloqueado pelo medidor: bateria abaixo de 10%. "
                            "Conecte o carregador antes de iniciar."
                        ),
                        text_color=DANGER,
                    )
                    self.test_state_label.configure(
                        text="BATERIA BAIXA", fg_color="#47232C", text_color=DANGER
                    )
                    self._update_controls()

                elif str(payload) == "ABORT_BT_DISCONNECT":
                    self.abort_reason = (
                        "O medidor informa que o último ensaio foi abortado por perda de Bluetooth."
                    )
                    self.footer_status.configure(text=self.abort_reason, text_color=DANGER)
                    self.test_state_label.configure(
                        text="ABORTADO", fg_color="#47232C", text_color=DANGER
                    )
                else:
                    self.footer_status.configure(
                        text=f"Status do medidor: {payload}", text_color=MUTED
                    )

            elif event == "warning":
                self.footer_status.configure(text=str(payload), text_color=WARNING)

            elif event == "error":
                self.footer_status.configure(text=str(payload), text_color=DANGER)
                self.connect_btn.configure(text="Tentar novamente", state="normal")
                if not self.connected:
                    self._clear_battery_status()

        self.after(100, self._poll_events)

    def _refresh_ui(self):
        source = self.all_samples if self.all_samples else self.samples
        if source:
            last = source[-1]
            self.temp_card.set(f"{last.temperature_c:.3f}")

            k_display, rho_display = self._corrected_result_values()
            if rho_display is not None:
                self.rho_card.set(f"{rho_display:.4f}")
            else:
                self.rho_card.set("--")

            if k_display is not None:
                self.k_card.set(f"{k_display:.4f}")
            else:
                self.k_card.set("--")

            if self.analysis.r2 is not None:
                self.r2_card.set(f"{self.analysis.r2:.5f}")
            else:
                self.r2_card.set("--")

            q_txt = f"{self.analysis.q_w_m:.1f}" if self.analysis.q_w_m is not None else "--"
            p_txt = f"{self.analysis.mean_power_w:.4f}" if self.analysis.mean_power_w is not None else "--"
            st_txt = (
                f"{self.analysis.power_stability_pct:.2f}"
                if self.analysis.power_stability_pct is not None else "--"
            )
            phase_label = PHASE_LABELS.get(self.current_phase, self.current_phase)
            self.metrics_footer.configure(
                text=(
                    f"fase: {phase_label}  |  q: {q_txt} W/m  |  P: {p_txt} W  |  "
                    f"estabilidade: ±{st_txt}%  |  t fase: {last.time_s:.0f} s"
                )
            )

            if self.warning_reason and self.running:
                self.footer_status.configure(text=self.warning_reason, text_color=WARNING)

            self._update_phase_ui()

        self._draw_plot()
        self.after(250, self._refresh_ui)

    def _update_controls(self):
        battery_blocks_start = (
            self.battery_valid
            and self.battery_percent is not None
            and self.battery_percent < 10
        )

        can_start = self.connected and not self.running and not battery_blocks_start

        self.start_btn.configure(state="normal" if can_start else "disabled")
        self.stop_btn.configure(state="normal" if self.running else "disabled")
        self.disconnect_btn.configure(state="normal" if self.connected else "disabled")
        self.export_btn.configure(state="normal" if self.all_samples or self.samples else "disabled")
        self._refresh_calibration_tab()

    # ------------------------------------------------------------- EXPORT ----
    def export_excel(self):
        if not self.all_samples and not self.samples:
            messagebox.showinfo("Exportação", "Ainda não existem dados para exportar.")
            return

        default_name = f"ensaio_ieee442_{datetime.now():%Y%m%d_%H%M%S}.xlsx"
        path = filedialog.asksaveasfilename(
            title="Exportar ensaio IEEE 442 para Excel",
            defaultextension=".xlsx",
            initialfile=default_name,
            filetypes=[("Planilha Excel", "*.xlsx")],
        )
        if not path:
            return

        try:
            self._calculate_analysis()
            assessment = self._final_software_assessment()
            k_display, rho_display = self._corrected_result_values()
            profile = self.profile_menu.get()
            wb = Workbook()
            ws_summary = wb.active
            ws_summary.title = "Resumo IEEE 442"
            ws_data = wb.create_sheet("Dados brutos")
            ws_protocol = wb.create_sheet("Pontos do protocolo")

            ws_summary["A1"] = "TLS METER — ENSAIO DE RESISTIVIDADE TÉRMICA"
            ws_summary["A1"].font = Font(size=16, bold=True, color="FFFFFF")
            ws_summary["A1"].fill = PatternFill("solid", fgColor="17365D")
            ws_summary.merge_cells("A1:B1")
            ws_summary["A2"] = f"Referência de software: {STANDARD_NAME}"
            ws_summary["A2"].font = Font(italic=True, color="666666")
            ws_summary.merge_cells("A2:B2")

            try:
                length_mm = self._float_entry(self.length_entry)
                diameter_mm = self._float_entry(self.diameter_entry)
                ratio = length_mm / diameter_mm
            except Exception:
                length_mm = None
                diameter_mm = None
                ratio = None

            # Temperatura de referência obtida automaticamente na estabilização.
            reference_t0 = self.baseline_temperature_c

            export_source = self.all_samples if self.all_samples else self.samples
            max_temp = max(s.temperature_c for s in export_source)
            max_delta = max_temp - reference_t0 if reference_t0 is not None else None
            final_temp = export_source[-1].temperature_c if export_source else None
            cooling_recovery = (
                final_temp - self.baseline_temperature_c
                if final_temp is not None and self.baseline_temperature_c is not None else None
            )

            summary_rows = [
                ("Data/hora da exportação", datetime.now().strftime("%d/%m/%Y %H:%M:%S")),
                ("Perfil", profile),
                ("Avaliação do software", assessment),
                ("Identificação da amostra", self.material_entry.get()),
                ("Temperatura de referência T0 — automática (°C)", reference_t0),
                ("Temperatura máxima da sonda (°C)", max_temp),
                ("Elevação máxima ΔT (°C)", max_delta),
                ("Comprimento ativo L (mm)", length_mm),
                ("Diâmetro da sonda D (mm)", diameter_mm),
                ("Relação L/D", ratio),
                ("Diâmetro interno do recipiente (mm)", self.container_entry.get()),
                ("Equilíbrio térmico prévio confirmado", "Sim — verificação rápida" if self.equilibrium_var.get() else "Não — estabilização pelo programa"),
                ("Fase final", PHASE_LABELS.get(self.current_phase, self.current_phase)),
                ("T0 calculada pela média final da estabilização (°C)", self.baseline_temperature_c),
                ("Taxa de deriva (dT/dt) ao fim da estabilização (°C/min)", self.stabilization_drift_c_min),
                ("Amostras de estabilização", len(self.stabilization_samples)),
                ("Duração de aquecimento adquirida (s)", self.samples[-1].time_s if self.samples else 0),
                ("Amostras de aquecimento", len(self.samples)),
                ("Amostras de resfriamento", len(self.cooling_samples)),
                ("Temperatura ao fim do ciclo (°C)", final_temp),
                ("Diferença final para linha de base (°C)", cooling_recovery),
                ("Número de amostras brutas", len(self.samples)),
                ("Pontos no protocolo de registro", self.analysis.recorded_points),
                ("Janela de regressão automática", "Sim" if self.analysis.automatic_window else "Não"),
                ("Início da janela usada (s)", self.analysis.fit_start_s),
                ("Fim da janela usada (s)", self.analysis.fit_end_s),
                ("Amplitude logarítmica (ciclos)", self.analysis.log_span_decades),
                ("Pontos usados na regressão", self.analysis.points_used),
                ("Inclinação dT/dln(t) (K)", self.analysis.slope_ln_k),
                ("ΔT por ciclo log10 (°C/década)", self.analysis.delta_t_decade_c),
                ("R² da regressão", self.analysis.r2),
                ("Potência média aplicada (W)", self.analysis.mean_power_w),
                ("Estabilidade máxima da potência (%)", self.analysis.power_stability_pct),
                ("Potência linear q (W/m)", self.analysis.q_w_m),
                ("Resistividade térmica ρ BRUTA (K·m/W)", self.analysis.resistivity_km_w),
                ("Condutividade térmica k BRUTA [W/(m·K)]", self.analysis.conductivity_w_mk),
                ("Resistividade térmica ρ apresentada (K·m/W)", rho_display),
                ("Condutividade térmica k apresentada [W/(m·K)]", k_display),
                ("Fator de calibração aplicado neste ciclo", self.run_factor),
                ("Ensaio destinado à calibração", "Sim" if self.calibration_run else "Não"),
                ("Referência k informada no ensaio de calibração", self.calibration_run_reference if self.calibration_run else None),
                ("Amostra Padrão identificada", self.calibration_run_sample_id if self.calibration_run else None),
                ("Identificação do fator salvo no início do ciclo", self.run_calibration_snapshot.sample_id),
                ("Data do fator salvo no início do ciclo", self.run_calibration_snapshot.created_at),
                ("Motivo de aborto", self.abort_reason),
                ("Aviso durante ensaio", self.warning_reason),
            ]

            for row_idx, (label, value) in enumerate(summary_rows, start=4):
                ws_summary.cell(row=row_idx, column=1, value=label).font = Font(bold=True)
                ws_summary.cell(row=row_idx, column=2, value=value)
            ws_summary.column_dimensions["A"].width = 45
            ws_summary.column_dimensions["B"].width = 30

            # Dados brutos do ciclo completo
            headers = [
                "Fase", "Tempo do ciclo (s)", "Tempo da fase (s)",
                "Temperatura (°C)", "Potência (W)", "ln(t aquecimento)", "log10(t aquecimento)"
            ]
            for col, header in enumerate(headers, start=1):
                cell = ws_data.cell(row=1, column=col, value=header)
                cell.font = Font(bold=True, color="FFFFFF")
                cell.fill = PatternFill("solid", fgColor="17365D")
                cell.alignment = Alignment(horizontal="center")

            export_source = self.all_samples if self.all_samples else self.samples
            for row_idx, sample in enumerate(export_source, start=2):
                ws_data.cell(row=row_idx, column=1, value=sample.phase)
                ws_data.cell(row=row_idx, column=2, value=sample.session_time_s)
                ws_data.cell(row=row_idx, column=3, value=sample.time_s)
                ws_data.cell(row=row_idx, column=4, value=sample.temperature_c)
                ws_data.cell(row=row_idx, column=5, value=sample.power_w)
                if sample.phase == PHASE_HEATING and sample.time_s > 0:
                    ws_data.cell(row=row_idx, column=6, value=math.log(sample.time_s))
                    ws_data.cell(row=row_idx, column=7, value=math.log10(sample.time_s))
            ws_data.freeze_panes = "A2"
            for col in "ABCDEFG":
                ws_data.column_dimensions[col].width = 22

            # Protocol points and fit membership
            protocol_headers = ["Tempo (s)", "Temperatura (°C)", "Potência (W)", "Usado na regressão"]
            for col, header in enumerate(protocol_headers, start=1):
                cell = ws_protocol.cell(row=1, column=col, value=header)
                cell.font = Font(bold=True, color="FFFFFF")
                cell.fill = PatternFill("solid", fgColor="17365D")
            fit_ids = {(round(s.time_s, 6), round(s.temperature_c, 6)) for s in self.fit_samples}
            for row_idx, sample in enumerate(self.analysis_samples, start=2):
                ws_protocol.cell(row=row_idx, column=1, value=sample.time_s)
                ws_protocol.cell(row=row_idx, column=2, value=sample.temperature_c)
                ws_protocol.cell(row=row_idx, column=3, value=sample.power_w)
                used = (round(sample.time_s, 6), round(sample.temperature_c, 6)) in fit_ids
                ws_protocol.cell(row=row_idx, column=4, value="Sim" if used else "Não")
            ws_protocol.freeze_panes = "A2"
            for col in "ABCD":
                ws_protocol.column_dimensions[col].width = 22

            # Temperature chart
            chart = LineChart()
            chart.title = "Temperatura durante o ensaio"
            chart.y_axis.title = "Temperatura (°C)"
            chart.x_axis.title = "Tempo (s)"
            chart_rows = len(export_source) + 1
            data_ref = Reference(ws_data, min_col=4, min_row=1, max_row=chart_rows)
            time_ref = Reference(ws_data, min_col=2, min_row=2, max_row=chart_rows)
            chart.add_data(data_ref, titles_from_data=True)
            chart.set_categories(time_ref)
            chart.height = 8
            chart.width = 14
            ws_summary.add_chart(chart, "D4")

            wb.save(path)
            self.footer_status.configure(text=f"Planilha exportada: {Path(path).name}", text_color=MUTED)
            messagebox.showinfo("Exportação concluída", f"Arquivo salvo com sucesso:\n{path}")
        except Exception as exc:
            messagebox.showerror("Erro ao exportar", f"Não foi possível gerar a planilha:\n{exc}")

    # --------------------------------------------------------------- CLOSE --
    def on_close(self):
        try:
            if self.connected:
                self.bt.request_disconnect()
        finally:
            self.after(150, self.destroy)


if __name__ == "__main__":
    app = TLSApp()
    app.mainloop()
