"""Verificações da duração do ensaio, independentes da interface gráfica.

Pacote DATA: session_time_s = tempo desde o começo da sessão;
             time_s = tempo desde o começo da fase atual.
"""
from typing import Iterable, Tuple


def heating_duration_from_samples(heating_samples: Iterable, cooling_samples: Iterable) -> Tuple[float, bool]:
    """Retorna (duração_s, confirmada_pela_transição_de_fase).

    O último pacote de aquecimento indica o último instante AMOSTRADO, não
    a duração total comandada. Se existe uma amostra de resfriamento, o
    início de cada fase pode ser reconstruído por (t_sessão - t_fase).

    Em ausência de confirmação, retorna só o último instante observado,
    com confirmed=False. Não substitui o tempo real por 600 s solicitado.
    """
    heating = list(heating_samples)
    cooling = list(cooling_samples)
    last_observed = max((s.time_s for s in heating), default=0.0)

    if heating and cooling:
        # Pacotes do protocolo V4/V5 têm o campo da fase explícito.
        h = next((s for s in heating if getattr(s, "phase", None) == "HEATING"), None)
        c = next((s for s in cooling if getattr(s, "phase", None) == "COOLING"), None)
        if h is not None and c is not None:
            heat_started_at = h.session_time_s - h.time_s
            cool_started_at = c.session_time_s - c.time_s
            actual = cool_started_at - heat_started_at
            # Coerência mínima e margem para telemetria; sem mascarar ciclo
            # realmente mais curto. Nunca confiar em valores não positivos.
            if actual > 0 and actual + 1.0 >= last_observed and actual < 86400:
                return (actual, True)

    return (last_observed, False)
