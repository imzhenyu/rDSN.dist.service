scripts_path=`readlink -f $0`
package_dir=`dirname $scripts_path`
echo "package_dir = $package_dir" >> run.txt
echo "port = $1" >> run.txt

$DSN_ROOT/bin/dsn.svchost $package_dir/config.ini -cargs port=$1

