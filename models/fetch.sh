#!/bin/sh
# Fetches the Smithy models of the services below from aws/api-models-aws at the commit below, as <service>.json.
set -eu
commit=3678d8a26e38bf4855aff5bae7be447ca52e63a0
cd "$(dirname "$0")"
for model in sts/service/2011-06-15/sts-2011-06-15.json iam/service/2010-05-08/iam-2010-05-08.json \
	sqs/service/2012-11-05/sqs-2012-11-05.json; do
	curl -fsS -A curl -o "${model%%/*}.json" "https://raw.githubusercontent.com/aws/api-models-aws/$commit/models/$model"
done
