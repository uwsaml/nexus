import os
import argparse
import subprocess
import yaml

_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '../..'))
_profiler = os.path.join(_root, 'build/bin/profiler')
_models = {}


def load_model_db(path):
    with open(path) as f:
        model_db = yaml.load(f.read())
    for model_info in model_db['models']:
        framework = model_info['framework']
        model_name = model_info['model_name']
        if framework not in _models:
            _models[framework] = {}
        _models[framework][model_name] = model_info


def find_max_batch(framework, model_name):
    global args
    cmd_base = '%s -model_root %s -image_dir %s -gpu %s -framework %s -model %s' % (
        _profiler, args.model_dir, args.dataset, args.gpu, framework, model_name)
    if args.height > 0 and args.width > 0:
        cmd_base += ' -height %s -width %s' % (args.height, args.width)
    left = 1
    right = 64
    curr_tp = None
    out_of_memory = False
    while True:
        prev_tp = curr_tp
        curr_tp = None
        cmd = cmd_base + ' -min_batch %s -max_batch %s' % (right, right)
        print(cmd)
        proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        out, err = proc.communicate()
        if 'out of memory' in err or 'out of memory' in out:
            print('batch %s: out of memory' % right)
            out_of_memory = True
            break
        flag = False
        for line in out.split('\n'):
            if flag:
                items = line.split(',')
                lat = float(items[1]) + float(items[2]) # mean + std
                curr_tp = right * 1e6 / lat
                break
            if line.startswith('batch,latency'):
                flag = True
        if curr_tp is None:
            # Unknown error happens, need to fix first
            print(err)
            exit(1)
        print('batch %s: throughput %s' % (right, curr_tp))
        if prev_tp is not None and curr_tp / prev_tp < 1.01:
            break
        if right == 1024:
            break
        left = right
        right *= 2
    if not out_of_memory:
        return right
    while right - left > 1:
        print(left, right)
        mid = (left + right) / 2
        cmd = cmd_base + ' -min_batch %s -max_batch %s' % (mid, mid)
        print(cmd)
        proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        out, err = proc.communicate()
        if 'out of memory' in err or 'out of memory' in out:
            right = mid
        else:
            left = mid
    return left


def profile_model(framework, model_name):
    global args
    prof_id = '%s:%s:%s' % (framework, model_name, args.version)
    if args.height > 0 and args.width > 0:
        prof_id += ':%sx%s' % (args.height, args.width)
    print('Profile %s' % prof_id)

    max_batch = find_max_batch(framework, model_name)
    print('Max batch: %s' % max_batch)

    output = prof_id + '.txt'
    cmd = '%s -model_root %s -image_dir %s -gpu %s -framework %s -model %s -max_batch %s -output %s' % (
        _profiler, args.model_dir, args.dataset, args.gpu, framework, model_name,
        max_batch, output)
    if args.height > 0 and args.width > 0:
        cmd += ' -height %s -width %s' % (args.height, args.width)
    print(cmd)

    proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    out, err = proc.communicate()
    if not os.path.exists(output):
        lines = err.split('\n')
        print('\n'.join(lines[-50:]))


def main():
    parser = argparse.ArgumentParser(description='Profile models')
    parser.add_argument('-f', '--framework',
                        choices=['caffe', 'caffe2', 'tensorflow', 'darknet'],
                        help='Framework name')
    parser.add_argument('-m', '--model', type=str, help='Model name')
    parser.add_argument('-v', '--version', type=int, default=1,
                        help='Model version')
    parser.add_argument('--gpu', type=int, default=0, help='GPU index')
    parser.add_argument('--dataset', type=str,
                        default='/home/haichen/datasets/imagenet/ILSVRC2012/val',
                        help='Dataset directory')
    parser.add_argument('--model_dir', type=str,
                        default='/home/haichen/nexus-models',
                        help='Model root directory')
    parser.add_argument('--height', type=int, default=0, help='Image height')
    parser.add_argument('--width', type=int, default=0, help='Image width')
    global args
    args = parser.parse_args()

    load_model_db(os.path.join(args.model_dir, 'db', 'model_db.yml'))
    if args.framework:
        frameworks = [args.framework]
    else:
        frameworks = _models.keys()
    for framework in frameworks:
        if args.model:
            if args.model not in _models[framework]:
                continue
            profile_model(framework, args.model)
        else:
            for model in _models[framework]:
                profile_model(framework, model)
    

if __name__ == '__main__':
    main()
