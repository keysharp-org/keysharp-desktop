#!/bin/sh
set -eu

source_dir=${1:?source directory is required}
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

expected_client_abi_major=$(awk '$2 == "KSD_CLIENT_ABI_MAJOR" { gsub(/u$/, "", $3); print $3 }' \
    "$source_dir/include/keysharp_desktop/client.h")
expected_client_abi_minor=$(awk '$2 == "KSD_CLIENT_ABI_MINOR" { gsub(/u$/, "", $3); print $3 }' \
    "$source_dir/include/keysharp_desktop/client.h")
for component in major minor; do
    installer_value=$(sed -n "s/^expected_client_abi_${component}=//p" \
        "$source_dir/packaging/install-release.sh")
    if [ "$component" = major ]; then
        header_value=$expected_client_abi_major
    else
        header_value=$expected_client_abi_minor
    fi
    if [ "$installer_value" != "$header_value" ]; then
        echo "archive installer ABI $component does not match its public header" >&2
        exit 1
    fi
done

sed -n '/^component_contract_matches() {$/,/^}$/p' \
    "$source_dir/packaging/install-release.sh" > "$temporary/contract.sh"
# shellcheck source=/dev/null
. "$temporary/contract.sh"

check_contract() {
    check_major=$1
    check_minor=$2
    expected=$3
    printf '%s\n' '#!/bin/sh' \
        "printf '%s\\n' client_abi_major=$check_major client_abi_minor=$check_minor" \
        > "$temporary/info"
    chmod 0755 "$temporary/info"
    actual=rejected
    if component_contract_matches "$temporary/info"; then
        actual=accepted
    fi
    if [ "$actual" != "$expected" ]; then
        echo "ABI $check_major.$check_minor was $actual; expected $expected" >&2
        exit 1
    fi
}

check_contract "$expected_client_abi_major" "$expected_client_abi_minor" accepted
check_contract "$expected_client_abi_major" "$((expected_client_abi_minor + 1))" accepted
check_contract "$expected_client_abi_major" "$((expected_client_abi_minor - 1))" rejected
check_contract "$((expected_client_abi_major + 1))" "$expected_client_abi_minor" rejected
check_contract "$expected_client_abi_major" invalid rejected

echo "keysharp-desktop installer ABI contract passed"
