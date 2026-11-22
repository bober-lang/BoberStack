#!/bin/sh
# Builds awsjson11-tests.json: Smithy's awsJson 1.1 protocol tests, the service aws.protocoltests.json#JsonProtocol and
# the shapes it reaches, their test vectors among its traits, in the JSON AST. It runs the Smithy CLI of the version
# below on Smithy's sources at its tag, in a directory of its own that it then removes.
set -eu
version=1.75.0 # commit 981f95a2cae567752b70749c1f04502c076a7a63
cli=smithy-cli-$(uname -s | tr A-Z a-z)-$(uname -m | sed 's/arm64/aarch64/')
models=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
curl -fsSL -A curl -o cli.zip "https://github.com/smithy-lang/smithy/releases/download/$version/$cli.zip"
unzip -q cli.zip
curl -fsSL -A curl "https://github.com/smithy-lang/smithy/archive/refs/tags/$version.tar.gz" | tar -xz
cat >smithy-build.json <<EOF
{
	"version": "1.0",
	"sources": [
		"smithy-$version/smithy-aws-protocol-tests/model/awsJson1_1",
		"smithy-$version/smithy-aws-protocol-tests/model/shared-types.smithy",
		"smithy-$version/smithy-aws-protocol-tests/model/aws-config.smithy",
		"smithy-$version/smithy-aws-traits/src/main/resources/META-INF/smithy",
		"smithy-$version/smithy-protocol-test-traits/src/main/resources/META-INF/smithy",
		"smithy-$version/smithy-protocol-traits/src/main/resources/META-INF/smithy",
		"smithy-$version/smithy-validation-model/model"
	],
	"projections": {
		"json11": {
			"transforms": [
				{"name": "includeServices", "args": {"services": ["aws.protocoltests.json#JsonProtocol"]}},
				{"name": "flattenAndRemoveMixins"},
				{"name": "removeUnusedShapes"}
			],
			"plugins": {"model": {}}
		}
	}
}
EOF
"$cli/bin/smithy" build --quiet -c smithy-build.json --output out
cp out/json11/model/model.json "$models/awsjson11-tests.json"
