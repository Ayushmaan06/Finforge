# syntax=docker/dockerfile:1
# FinForge API / worker image (PLAN Phase 9). One image, several commands:
#   finforge server api --host 0.0.0.0 | worker | migrate | demo
# Multi-stage: the build stage compiles the C++ engine into a wheel; the runtime stage installs only
# that wheel and its dependencies (no compiler, no sources).
ARG PYTHON=3.12-slim-trixie

FROM python:${PYTHON} AS build
# GCC 14 (Debian trixie). CMake and Ninja come from PyPI: scikit-build-core adds them to the build.
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json pyproject.toml README.md LICENSE ./
COPY engine engine
COPY python python
# No .git in the image: the engine version's git SHA is passed in (PLAN D6).
ARG GIT_SHA=unknown
RUN SKBUILD_CMAKE_DEFINE="FINFORGE_GIT_SHA=${GIT_SHA}" pip wheel --no-deps . -w /wheels

FROM python:${PYTHON}
COPY --from=build /wheels /wheels
RUN pip install --no-cache-dir "$(ls /wheels/finforge-*.whl)[server]" \
    && rm -rf /wheels \
    && useradd --create-home --uid 10001 finforge \
    && mkdir /data && chown finforge /data
USER finforge
ENV FINFORGE_STORAGE_DIR=/data PYTHONUNBUFFERED=1
VOLUME /data
EXPOSE 8000
# 0.0.0.0 inside the container; docker-compose.yml publishes the port on 127.0.0.1 only.
CMD ["finforge", "server", "api", "--host", "0.0.0.0", "--port", "8000"]
