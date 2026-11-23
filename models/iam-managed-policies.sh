#!/bin/sh
# Builds iam-managed-policies.jsonl: the AWS managed policies, every version of each, as the account the aws CLI is
# signed in to sees them, by IAM's read-only list-policies, get-policy, list-policy-versions and get-policy-version.
# A policy is a line, of its members as GetPolicy answers them but those of the account (AttachmentCount,
# PermissionsBoundaryUsageCount) and its Versions as ListPolicyVersions answers them; the document of each version
# follows it in that order, a line each, compact, as the CLI decodes it. Policies sort by their names lower-cased.
set -eu
jobs=32
export AWS_PAGER= AWS_RETRY_MODE=adaptive AWS_MAX_ATTEMPTS=20
models=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
aws iam list-policies --scope AWS --output json | jq -r '.Policies[].Arn' >arns
# A policy's line, as <name>.
xargs -P $jobs -n 1 sh -c '
	name=${0##*/}
	aws iam get-policy --policy-arn "$0" --output json >"$name.policy"
	aws iam list-policy-versions --policy-arn "$0" --output json >"$name.versions"
	jq -c --slurpfile v "$name.versions" ".Policy | {PolicyName, PolicyId, Arn, Path, DefaultVersionId, IsAttachable,
		Description, CreateDate, UpdateDate, Versions: [\$v[0].Versions[] | {VersionId, IsDefaultVersion, CreateDate}]}" \
		"$name.policy" >"$name"
	jq -r ".Versions[].VersionId | \"$0 \" + ." "$name" >"$name.ids"
' <arns
# A version's document, as <name>.<version>.
cat ./*.ids | xargs -P $jobs -n 2 sh -c '
	aws iam get-policy-version --policy-arn "$0" --version-id "$1" --output json |
		jq -c .PolicyVersion.Document >"${0##*/}.$1"
'
sed 's|.*/||' arns | awk '{ print tolower($0) "\t" $0 }' | LC_ALL=C sort | cut -f 2 | while read -r name; do
	cat "$name"
	for v in $(jq -r '.Versions[].VersionId' "$name"); do
		cat "$name.$v"
	done
done >"$models/iam-managed-policies.jsonl"
