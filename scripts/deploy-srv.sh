#!/usr/bin/env bash
set -Eeuo pipefail

if test "$#" -ne 1; then
    printf 'Usage: %s <commit-sha>\n' "$0" >&2
    exit 2
fi

COMMIT_SHA="$1"
REPOSITORY_ROOT="$(git rev-parse --show-toplevel)"
COMPOSE_FILE="$REPOSITORY_ROOT/deploy/docker-compose.srv.yml"
SERVICE_ROOT="${DAWN_SERVICE_ROOT:-/home/andrei/services/dawn}"
DEPLOY_ENV_FILE="${DAWN_DEPLOY_ENV_FILE:-$SERVICE_ROOT/config/deploy.env}"
IMAGE_REPOSITORY="${DAWN_IMAGE_REPOSITORY:-dawn}"
DAWN_CONTAINER_NAME="${DAWN_CONTAINER_NAME:-dawn}"
INGRESS_CONTAINER_NAME="${DAWN_INGRESS_CONTAINER_NAME:-dawn-ingress}"
POC_CONTAINER_NAME="${DAWN_POC_CONTAINER_NAME:-dawn-poc}"
INFRA_NETWORK="${DAWN_INFRA_NETWORK:-dawn_infra}"
INGRESS_ADDRESS="${DAWN_INGRESS_ADDRESS:-10.0.254.230}"
INGRESS_HOSTNAME="${DAWN_INGRESS_HOSTNAME:-jarvis.abpfa.network}"
OLLAMA_ENDPOINT="${DAWN_OLLAMA_ENDPOINT:-http://10.0.248.128:11434}"
OLLAMA_MODEL="${DAWN_OLLAMA_MODEL:-gemma4:12b}"
NEW_DAWN_IMAGE="${IMAGE_REPOSITORY}:${COMMIT_SHA}"
NEW_INGRESS_IMAGE="${IMAGE_REPOSITORY}-ingress:${COMMIT_SHA}"
STARTED_AT="$(date +%s)"
PREVIOUS_DAWN_IMAGE=""
PREVIOUS_INGRESS_IMAGE=""
POC_WAS_STOPPED=0
DEPLOYMENT_STARTED=0

compose() {
    local dawn_image="$1"
    local ingress_image="$2"
    shift 2

    DAWN_COMMIT_SHA="$COMMIT_SHA" \
    DAWN_IMAGE="$dawn_image" \
    DAWN_INGRESS_IMAGE="$ingress_image" \
    DAWN_SERVICE_ROOT="$SERVICE_ROOT" \
        docker compose \
        --env-file "$DEPLOY_ENV_FILE" \
        --file "$COMPOSE_FILE" \
        "$@"
}

network_value() {
    local template="$1"

    docker network inspect "$INFRA_NETWORK" --format "$template"
}

check_network() {
    test "$(network_value '{{.Driver}}')" = "ipvlan"
    test "$(network_value '{{index .Options "parent"}}')" = "enp2s0.254"
    test "$(network_value '{{(index .IPAM.Config 0).Subnet}}')" = "10.0.254.0/24"
    test "$(network_value '{{(index .IPAM.Config 0).IPRange}}')" = "10.0.254.224/27"
    test "$(network_value '{{(index .IPAM.Config 0).Gateway}}')" = "10.0.254.1"
}

check_ollama() {
    local payload

    payload="$(
        curl \
            --fail-with-body \
            --silent \
            --show-error \
            --max-time 15 \
            "${OLLAMA_ENDPOINT}/api/tags"
    )"

    OLLAMA_PAYLOAD="$payload" OLLAMA_MODEL="$OLLAMA_MODEL" python3 -c '
import json
import os

models = {
    item["name"]
    for item in json.loads(os.environ["OLLAMA_PAYLOAD"])["models"]
}
required = os.environ["OLLAMA_MODEL"]

if required not in models:
    raise SystemExit(f"Required Ollama model is unavailable: {required}")

print(f"Ollama model available: {required}")
'
}

wait_for_container_health() {
    local container_name="$1"
    local attempt
    local health

    for attempt in $(seq 1 90); do
        health="$(
            docker inspect "$container_name" \
                --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}missing{{end}}' \
                2>/dev/null || true
        )"

        if test "$health" = "healthy"; then
            return 0
        fi

        if test "$health" = "unhealthy"; then
            docker logs --tail 160 "$container_name" >&2 || true
            return 1
        fi

        sleep 2
    done

    docker logs --tail 160 "$container_name" >&2 || true
    return 1
}

wait_for_https() {
    local attempt

    for attempt in $(seq 1 90); do
        if curl \
            --fail-with-body \
            --silent \
            --show-error \
            --max-time 10 \
            --resolve "${INGRESS_HOSTNAME}:443:${INGRESS_ADDRESS}" \
            "https://${INGRESS_HOSTNAME}/" \
            >/dev/null 2>&1; then
            return 0
        fi

        sleep 2
    done

    docker logs --tail 200 "$INGRESS_CONTAINER_NAME" >&2 || true
    return 1
}

rollback() {
    local result=$?
    trap - ERR

    if test "$DEPLOYMENT_STARTED" -eq 1; then
        printf 'Deployment failed; starting rollback.\n' >&2

        if test -n "$PREVIOUS_DAWN_IMAGE" &&
            test -n "$PREVIOUS_INGRESS_IMAGE" &&
            docker image inspect "$PREVIOUS_DAWN_IMAGE" >/dev/null 2>&1 &&
            docker image inspect "$PREVIOUS_INGRESS_IMAGE" >/dev/null 2>&1; then
            compose \
                "$PREVIOUS_DAWN_IMAGE" \
                "$PREVIOUS_INGRESS_IMAGE" \
                up \
                --detach \
                --remove-orphans

            wait_for_container_health "$DAWN_CONTAINER_NAME"
            wait_for_container_health "$INGRESS_CONTAINER_NAME"

            printf \
                'Rollback restored DAWN image %s and ingress image %s.\n' \
                "$PREVIOUS_DAWN_IMAGE" \
                "$PREVIOUS_INGRESS_IMAGE" \
                >&2
        else
            compose \
                "$NEW_DAWN_IMAGE" \
                "$NEW_INGRESS_IMAGE" \
                down \
                --remove-orphans ||
                true

            if test "$POC_WAS_STOPPED" -eq 1; then
                docker start "$POC_CONTAINER_NAME" >/dev/null
                printf 'Rollback restored %s.\n' "$POC_CONTAINER_NAME" >&2
            fi
        fi
    fi

    exit "$result"
}
trap rollback ERR

cd "$REPOSITORY_ROOT"

if [[ ! "$COMMIT_SHA" =~ ^[0-9a-f]{40}$ ]]; then
    printf 'Invalid commit SHA: %s\n' "$COMMIT_SHA" >&2
    exit 2
fi

test "$(git rev-parse HEAD)" = "$COMMIT_SHA"
test -z "$(git status --porcelain=v1 --untracked-files=all)"
test -s "$SERVICE_ROOT/config/dawn.toml"
test -s "$SERVICE_ROOT/config/secrets.toml"
test -s "$SERVICE_ROOT/models/whisper.cpp/ggml-base.en.bin"
test -s "$DEPLOY_ENV_FILE"

grep -Eq \
    '^DAWN_CLOUDFLARE_DNS_API_TOKEN=.+$' \
    "$DEPLOY_ENV_FILE"

docker volume inspect dawn_data >/dev/null
docker volume inspect dawn_caddy_data >/dev/null
docker volume inspect dawn_caddy_config >/dev/null
docker network inspect "$INFRA_NETWORK" >/dev/null

check_network
check_ollama

compose \
    "$NEW_DAWN_IMAGE" \
    "$NEW_INGRESS_IMAGE" \
    config \
    --quiet

docker build \
    --label "org.opencontainers.image.revision=${COMMIT_SHA}" \
    --tag "$NEW_DAWN_IMAGE" \
    "$REPOSITORY_ROOT"

docker build \
    --file "$REPOSITORY_ROOT/deploy/ingress/Dockerfile" \
    --label "org.opencontainers.image.revision=${COMMIT_SHA}" \
    --tag "$NEW_INGRESS_IMAGE" \
    "$REPOSITORY_ROOT/deploy/ingress"

if docker inspect "$DAWN_CONTAINER_NAME" >/dev/null 2>&1; then
    PREVIOUS_DAWN_IMAGE="$(
        docker inspect \
            "$DAWN_CONTAINER_NAME" \
            --format '{{.Config.Image}}'
    )"
fi

if docker inspect "$INGRESS_CONTAINER_NAME" >/dev/null 2>&1; then
    PREVIOUS_INGRESS_IMAGE="$(
        docker inspect \
            "$INGRESS_CONTAINER_NAME" \
            --format '{{.Config.Image}}'
    )"
fi

if test "$(
    docker inspect "$POC_CONTAINER_NAME" \
        --format '{{.State.Running}}' \
        2>/dev/null || true
)" = "true"; then
    docker stop "$POC_CONTAINER_NAME" >/dev/null
    POC_WAS_STOPPED=1
fi

DEPLOYMENT_STARTED=1

compose \
    "$NEW_DAWN_IMAGE" \
    "$NEW_INGRESS_IMAGE" \
    up \
    --detach \
    --remove-orphans \
    --wait \
    --wait-timeout 180

wait_for_container_health "$DAWN_CONTAINER_NAME"
wait_for_container_health "$INGRESS_CONTAINER_NAME"

curl \
    --fail-with-body \
    --silent \
    --show-error \
    --max-time 10 \
    "http://${INGRESS_ADDRESS}/healthz" \
    >/dev/null

wait_for_https
check_ollama

docker exec "$DAWN_CONTAINER_NAME" sh -lc '
test -s /var/lib/dawn/.local/share/dawn/auth.db
test -s /var/lib/dawn/stat.db
'

DURATION_SECONDS="$(( $(date +%s) - STARTED_AT ))"

printf \
    'DAWN deployment healthy: image=%s ingress=%s duration=%ss\n' \
    "$NEW_DAWN_IMAGE" \
    "$NEW_INGRESS_IMAGE" \
    "$DURATION_SECONDS"

if test -n "${GITHUB_OUTPUT:-}"; then
    {
        printf 'duration_seconds=%s\n' "$DURATION_SECONDS"
        printf 'image=%s\n' "$NEW_DAWN_IMAGE"
        printf 'ingress_image=%s\n' "$NEW_INGRESS_IMAGE"
    } >> "$GITHUB_OUTPUT"
fi

trap - ERR
