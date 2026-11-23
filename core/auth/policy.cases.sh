#!/bin/sh
# Records in policy.cases.json the decision of IAM's policy simulator (iam simulate-custom-policy, which creates
# nothing) for each case, three calls at a time. Each case is of the caller, an IAM user, of the account that owns the
# resource or another; the real account's id, should a decision mention it, is recorded as 000000000000.
set -eu
cd "$(dirname "$0")"
export AWS_RETRY_MODE=adaptive AWS_MAX_ATTEMPTS=20 AWS_REGION=us-east-1
account=$(aws sts get-caller-identity --query Account --output text)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
n=$(jq length policy.cases.json)
i=0
while [ "$i" -lt "$n" ]; do
	jq --argjson i "$i" '.[$i] | {
		PolicyInputList: .policies,
		PermissionsBoundaryPolicyInputList: .boundary,
		CallerArn: .caller,
		ResourceOwner: "arn:aws:iam::\(.owner):root",
		ActionNames: [.action],
		ResourceArns: [.arn],
		ContextEntries: [.context[] | {ContextKeyName: .key, ContextKeyValues: .values, ContextKeyType: .type}]
	} + if .resource == "" then {} else {ResourcePolicy: .resource} end' policy.cases.json >"$dir/$i.in"
	i=$((i + 1))
done
# shellcheck disable=SC2016
seq 0 $((n - 1)) | xargs -P 3 -I % sh -c 'aws iam simulate-custom-policy --cli-input-json "file://$1/$2.in" \
	--query "EvaluationResults[0].EvalDecision" --output text >"$1/$2.out" || echo invalid >"$1/$2.out"' sh "$dir" %
for i in $(seq 0 $((n - 1))); do cat "$dir/$i.out"; done | sed "s/$account/000000000000/g" | jq -R . | jq -s . >"$dir/all"
if jq -e 'any(. == "invalid")' "$dir/all" >/dev/null; then
	jq -r --slurpfile d "$dir/all" 'to_entries[] | select($d[0][.key] == "invalid") | "invalid: \(.value.name)"' \
		policy.cases.json >&2
	exit 1
fi
jq --slurpfile d "$dir/all" --tab 'to_entries | map(.value + {decision: $d[0][.key]})' policy.cases.json \
	>"$dir/cases"
mv "$dir/cases" policy.cases.json
