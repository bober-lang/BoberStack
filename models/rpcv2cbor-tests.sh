#!/bin/sh
# Builds rpcv2cbor-tests.json and rpcv2cbor-querycompatible-tests.json: Smithy's RPC v2 CBOR protocol tests, the
# services smithy.protocoltests.rpcv2Cbor#RpcV2Protocol and aws.protocoltests.rpcv2cbor#QueryCompatibleRpcV2Protocol
# each with the shapes it reaches, their test vectors among its traits, in the JSON AST. It runs the Smithy CLI of the
# version below on Smithy's sources at its tag, in a directory of its own that it then removes. RpcV2Protocol is given
# a sigv4 trait, which it lacks and smithygen takes its signing name from.
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
cat >signed.smithy <<EOF
\$version: "2"
namespace smithy.protocoltests.rpcv2Cbor
use aws.auth#sigv4
apply RpcV2Protocol @sigv4(name: "rpcv2protocol")
EOF
cat >smithy-build.json <<EOF
{
	"version": "1.0",
	"sources": [
		"signed.smithy",
		"smithy-$version/smithy-protocol-tests/model/rpcv2Cbor",
		"smithy-$version/smithy-protocol-tests/model/shared-types.smithy",
		"smithy-$version/smithy-aws-protocol-tests/model/rpcv2Cbor",
		"smithy-$version/smithy-aws-protocol-tests/model/aws-config.smithy",
		"smithy-$version/smithy-aws-traits/src/main/resources/META-INF/smithy",
		"smithy-$version/smithy-protocol-test-traits/src/main/resources/META-INF/smithy",
		"smithy-$version/smithy-protocol-traits/src/main/resources/META-INF/smithy",
		"smithy-$version/smithy-validation-model/model"
	],
	"projections": {
		"rpcv2cbor": {
			"transforms": [
				{"name": "includeServices", "args": {"services": ["smithy.protocoltests.rpcv2Cbor#RpcV2Protocol"]}},
				{"name": "flattenAndRemoveMixins"},
				{"name": "removeUnusedShapes"}
			],
			"plugins": {"model": {}}
		},
		"querycompatible": {
			"transforms": [
				{"name": "includeServices",
					"args": {"services": ["aws.protocoltests.rpcv2cbor#QueryCompatibleRpcV2Protocol"]}},
				{"name": "flattenAndRemoveMixins"},
				{"name": "removeUnusedShapes"}
			],
			"plugins": {"model": {}}
		}
	}
}
EOF
"$cli/bin/smithy" build --quiet -c smithy-build.json --output out
cp out/rpcv2cbor/model/model.json "$models/rpcv2cbor-tests.json"
cp out/querycompatible/model/model.json "$models/rpcv2cbor-querycompatible-tests.json"
