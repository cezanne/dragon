import csv
import argparse
import os.path
import math

import matplotlib.pyplot as plt
from matplotlib.ticker import ScalarFormatter

import numpy as np
import scipy.stats

width = 0.15

BAR_NUM_FONTSIZE = 25

HATCHES = {
    'original': None,
    'uvm': '//',
    'cpu': None,
    'cpu-omp': '//',
    'dragon': '\\\\',
}

COLORS = {
    'original': (0.8, 0.8, 0.8,),
    'uvm': (0.8, 0.8, 0.8,),
    'cpu': (0.5, 0.5, 0.5,),
    'cpu-omp': (0.5, 0.5, 0.5,),
    'dragon': (0.3, 0.3, 0.3,),
}

LABELS = {
    'original': 'Default',
    'uvm': 'UM-P',
    'cpu': 'C++ ATLAS',
    'cpu-omp': 'C++ OPENBLAS',
    'dragon': 'DRAGON',
    'dragon exec time': 'DRAGON exec time',
}

def parseargs():
    parser = argparse.ArgumentParser(
        description = 'Benchmark result time comparison plotter for ResNet'
    )

    parser.add_argument(
        '--save',
        help = 'Output filename'
    )

    return parser.parse_args()

def plot_prog(name, ax):
    data_raw = {
        'original': dict(),
        'uvm': dict(),
        'cpu': dict(),
        'cpu-omp': dict(),
        'dragon': dict(),
    }

    memsize_map = dict()

    with open('../results/{}/memsize.data'.format(name), 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            model = row['model']
            size = int(row['size (B)'])
            memsize_map[model] = size

    for benchmark_type in ('original', 'uvm', 'cpu', 'cpu-omp', 'dragon',):
        exec_time_array = data_raw[benchmark_type]

        with open('../results/{}/result-{}.data'.format(name, benchmark_type), 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                model = row['model']
                if int(model.strip().split('_')[1]) < 32:
                    continue
                exec_time = float(row['total_time (s)'])

                assert exec_time > 0, '%s %s %d' % (name, benchmark_type, exec_time,)

                exec_time_array[model] = exec_time

    normalized_data = dict()
    for benchmark_type, time_data in data_raw.items():
        normalized_data[benchmark_type] = dict()
        for model, exec_time in time_data.items():
            normalized_data[benchmark_type][model] = exec_time / data_raw['dragon'][model]

    legends = dict()

    sorted_progs = ['original', 'uvm', 'cpu', 'cpu-omp', 'dragon',]
    num_progs = len(sorted_progs)
    i = 0
    for prog in sorted_progs:
        prog_data = normalized_data[prog]
        x_array = np.arange(len(prog_data)) + (i - (float(num_progs) / 2.0 - 0.5)) * width

        y_array = np.asarray([y for x, y in sorted(prog_data.items(), key = lambda item: int(item[0].split('_')[1]))])

        b = ax.bar(
            x_array,
            y_array,
            width,
            label = prog,
            hatch = HATCHES[prog],
            color = COLORS[prog],
            edgecolor = 'k'
        )

        legends[prog] = b

        for x, y in zip(x_array, y_array):
            if y >= 5.0:
                ax.text(x, min(y + 0.1, 5.1), '{:.2f}'.format(y), 
                    fontdict = {
                        'size': BAR_NUM_FONTSIZE,
                        'weight': 'bold',
                    },
                    ha = 'center',
                    rotation = 90,
                    va = 'bottom'
                )
        
        i += 1

    ax_right = ax.twinx()
    b = ax_right.plot(
        np.arange(len(prog_data)),
        np.asarray([y / 60.0 for x, y in sorted(data_raw['dragon'].items(), key = lambda item: int(item[0].split('_')[1]))]),
        color = 'k',
        linewidth = 5
    )

    legends['dragon exec time'] = b[0]

    ax_right.set_yscale('log')
    ax_right.yaxis.set_major_formatter(ScalarFormatter())

    for label in ax_right.get_yticklabels():
        label.set_weight('bold')
        label.set_size(25)

    ax.set_xticks(np.arange(len(data_raw['dragon'])))
    ax.set_xticklabels(
        ['{:s}\n[{:.2f}]'.format(model, memsize_map[model] / float(2 ** 30)) for model in sorted(data_raw['dragon'].keys(), key = lambda item: int(item.split('_')[1]))],
        fontdict = {
            'weight': 'bold',
            'size': 25,
        }
    )


    for label in ax.get_yticklabels():
        label.set_weight('bold')
        label.set_size(25)

    ax.set_ylim(top = 5)

    ax.set_xlabel("Model\n[Memory footprint (GiB)]", size = 35, weight = 'bold')
    ax.set_ylabel("Normalized Time", size = 35, weight = 'bold')
    ax_right.set_ylabel("Execution time (mins)", size = 35, weight = 'bold')

    #ax.set_title(name, size = 20, weight = 'bold')

    return legends

def main(args):
    fig, ax = plt.subplots()
    legends = plot_prog('resnet', ax)

    sorted_progs = ['original', 'uvm', 'cpu', 'cpu-omp', 'dragon', 'dragon exec time']
    sorted_legend_objs = list()
    for prog in sorted_progs:
        sorted_legend_objs.append(legends[prog])

    ax.legend(
        sorted_legend_objs,
        [LABELS[prog] for prog in sorted_progs],
        loc = "upper center", 
        ncol = 3,
        prop = {
            'size': 25,
            'weight': 'bold',
        },
        bbox_to_anchor = (0.5, 1.7)
    )

    if args.save:
        fig.set_size_inches(17, 5)
        plt.savefig(args.save, dpi = 200, bbox_inches = 'tight')
    else:
        plt.show()

if __name__ == '__main__':
    main(parseargs())
