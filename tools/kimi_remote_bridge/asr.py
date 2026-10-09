"""faster-whisper ASR wrapper.

Decodes 16-bit PCM from the device (16 kHz mono) and returns recognized
text.  Model size/path come from the bridge configuration; defaults target
Chinese speech on CPU with int8 quantization.
"""

from __future__ import annotations

import logging
from typing import Optional

log = logging.getLogger(__name__)


class ASRError(Exception):
    """Raised when transcription fails or input audio is unusable."""


class WhisperASR:
    def __init__(self, model_size: str = "small",
                 model_path: Optional[str] = None,
                 language: str = "zh",
                 device: str = "cpu",
                 compute_type: str = "int8",
                 initial_prompt: Optional[str] = "以下是普通话的句子。",
                 vad_filter: bool = False) -> None:
        try:
            import numpy  # noqa: F401
            from faster_whisper import WhisperModel
        except ImportError as exc:  # pragma: no cover - environment issue
            raise ASRError(
                f"faster-whisper is not available: {exc}") from exc
        self.language = language
        self.initial_prompt = initial_prompt
        self.vad_filter = vad_filter
        path = model_path or model_size
        log.info("加载 ASR 模型 %s (device=%s, compute_type=%s)",
                 path, device, compute_type)
        try:
            self._model = WhisperModel(path, device=device,
                                       compute_type=compute_type)
        except Exception as exc:
            raise ASRError(f"failed to load model {path}: {exc}") from exc
        self._numpy = numpy

    def transcribe(self, wav: bytes, sample_rate: int = 16000) -> str:
        """Transcribe a WAV container (16 kHz s16 mono) to text.

        Raises :class:`ASRError` on failure; returns "" for empty audio.
        """
        if len(wav) < 200:  #  header only / truncated: nothing to recognize
            log.warning("音频过短(%d 字节),跳过识别", len(wav))
            return ""
        kwargs = dict(
            language=self.language or None,
            beam_size=5,
            vad_filter=self.vad_filter,
            condition_on_previous_text=False,
        )
        if self.initial_prompt:
            kwargs["initial_prompt"] = self.initial_prompt
        try:
            import io
            segments, info = self._model.transcribe(io.BytesIO(wav), **kwargs)
            text = "".join(seg.text for seg in segments).strip()
        except Exception as exc:
            raise ASRError(f"transcribe failed: {exc}") from exc
        log.info("识别结果(%.2fs,%s):%s", info.duration,
                 info.language, text)
        return text
