#
# anomaly-injector sidecar: sets the anomaly label of the AURORA samples (anomaly_label column) of an OCUDU RAN
# container (gnb / du / cu / cu_cp / cu_up) and drives its AURORA anomaly module over its TCP control port, from a
# schedule or from other containers of the pod (control port 47020); see utils/anomaly_injector/anomaly_injector.py
# and lib/aurora/README.md.
#
# Build (from the repository root):
#   docker build -f docker/anomaly_injector/Dockerfile -t ocudu/anomaly_injector .
#
# Run in the network namespace of a RAN container:
#   docker run --rm --network container:ocudu_gnb \
#     -e ANOMALY_SCHEDULE=/etc/anomaly-injector/schedule.json \
#     -v $PWD/docker/anomaly_injector/schedule.example.json:/etc/anomaly-injector/schedule.json:ro \
#     ocudu/anomaly_injector
#
# Python standard library only: no packages are installed.

ARG PYTHON_VERSION=3.12-slim

FROM python:${PYTHON_VERSION}

LABEL summary="AURORA anomaly injector sidecar for OCUDU" \
      description="Sends anomaly schedules to the AURORA anomaly module of an OCUDU RAN process"

COPY utils/anomaly_injector/anomaly_injector.py /usr/local/bin/anomaly-injector
COPY docker/anomaly_injector/schedule.example.json /etc/anomaly-injector/schedule.example.json
RUN chmod 0755 /usr/local/bin/anomaly-injector && useradd --system --uid 10001 --no-create-home injector

USER 10001
ENV PYTHONUNBUFFERED=1 \
    AURORA_ANOMALY_HOST=127.0.0.1 \
    AURORA_ANOMALY_PORT=47010 \
    ANOMALY_INJECTOR_PORT=47020 \
    ANOMALY_LABEL_RESEND_S=1

ENTRYPOINT ["anomaly-injector"]
