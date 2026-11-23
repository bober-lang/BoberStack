#!/bin/sh
# Fetches the Smithy models of the services below from aws/api-models-aws at the commit below, as <service>.json, without its hyphens.
set -eu
commit=3678d8a26e38bf4855aff5bae7be447ca52e63a0
cd "$(dirname "$0")"
for model in sts/service/2011-06-15/sts-2011-06-15.json iam/service/2010-05-08/iam-2010-05-08.json \
	sqs/service/2012-11-05/sqs-2012-11-05.json dynamodb/service/2012-08-10/dynamodb-2012-08-10.json \
	dynamodb-streams/service/2012-08-10/dynamodb-streams-2012-08-10.json \
	sns/service/2010-03-31/sns-2010-03-31.json eventbridge/service/2015-10-07/eventbridge-2015-10-07.json \
	s3/service/2006-03-01/s3-2006-03-01.json secrets-manager/service/2017-10-17/secrets-manager-2017-10-17.json \
	ssm/service/2014-11-06/ssm-2014-11-06.json cloudwatch-logs/service/2014-03-28/cloudwatch-logs-2014-03-28.json \
	kms/service/2014-11-01/kms-2014-11-01.json kinesis/service/2013-12-02/kinesis-2013-12-02.json \
	lambda/service/2015-03-31/lambda-2015-03-31.json \
	sfn/service/2016-11-23/sfn-2016-11-23.json \
	route-53/service/2013-04-01/route-53-2013-04-01.json \
	cloudwatch/service/2010-08-01/cloudwatch-2010-08-01.json firehose/service/2015-08-04/firehose-2015-08-04.json \
	acm/service/2015-12-08/acm-2015-12-08.json ses/service/2010-12-01/ses-2010-12-01.json \
	sesv2/service/2019-09-27/sesv2-2019-09-27.json \
	cloudformation/service/2010-05-15/cloudformation-2010-05-15.json; do
	name=${model%%/*}
	curl -fsS -A curl -o "$(echo "$name" | tr -d -).json" "https://raw.githubusercontent.com/aws/api-models-aws/$commit/models/$model"
done
