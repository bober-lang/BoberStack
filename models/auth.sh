#!/bin/sh
# Fetches AWS's service authorization reference of each service below, by its signing name, as <service>-auth.json:
# the actions that authorize each operation, the resources' ARN formats and the condition keys, but not the SDKs that
# call each operation. The URL is not versioned; these were fetched on 2026-11-23.
set -eu
cd "$(dirname "$0")"
for service in iam sts sqs dynamodb sns events; do
	curl -fsS -A curl "https://servicereference.us-east-1.amazonaws.com/v1/$service/$service.json" \
		| jq "del(.Operations[].SDK)" >"$service-auth.json"
done
