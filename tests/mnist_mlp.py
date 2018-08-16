#!/usr/bin/env python3

"""An MNIST trainer which is exportable by ONNX-chainer."""

import argparse
import os
import sys

import numpy as np

import chainer
import chainer.functions as F
import chainer.links as L
from chainer import reporter
from chainer import training
from chainer.functions.evaluation import accuracy
from chainer.training import extensions
import onnx_chainer

sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from oniku.tools import npz_to_onnx


def replace_id(model, builtins=__builtins__):
    orig_id = id
    name_map = {}
    param_to_names = {}
    for name, param in model.namedparams():
        param_to_names[id(param)] = name

    def resolve_name(x):
        if orig_id(x) in param_to_names:
            return param_to_names[orig_id(x)]

        param_id = name_map.get(x.name, 0)
        name_map[x.name] = param_id + 1
        name = '%s_%d' % (x.name, param_id) if param_id else x.name
        return name

    def my_id(x):
        if (isinstance(x, chainer.Parameter) or
            isinstance(x, chainer.Variable) and x.name):
            if hasattr(x, 'onnx_name'):
                return x.onnx_name
            name = resolve_name(x)
            setattr(x, 'onnx_name', name)
            return name
        return orig_id(x)
    builtins.id = my_id


def makedirs(d):
    if not os.path.exists(d):
        os.makedirs(d)


class MyClassifier(chainer.link.Chain):
    """A Classifier which only supports 2D input."""

    def __init__(self, predictor, compute_accuracy):
        super(MyClassifier, self).__init__()
        self.compute_accuracy = compute_accuracy
        with self.init_scope():
            self.predictor = predictor

    def forward(self, x, t):
        y = self.predictor(x)
        log_softmax = F.log_softmax(y)
        # SelectItem is not supported by onnx-chainer.
        # TODO(hamaji): Support it?
        # log_prob = F.select_item(log_softmax, t)

        # TODO(hamaji): Currently, F.sum with axis=1 cannot be
        # backpropped properly.
        # log_prob = F.sum(log_softmax * t, axis=1)
        # self.batch_size = chainer.Variable(np.array(t.size, np.float32),
        #                                    name='batch_size')
        # return -F.sum(log_prob, axis=0) / self.batch_size
        log_prob = F.sum(log_softmax * t, axis=(0, 1))
        self.batch_size = chainer.Variable(np.array(t.size, np.float32),
                                           name='batch_size')
        loss = -log_prob / self.batch_size
        reporter.report({'loss': loss}, self)
        if self.compute_accuracy:
            acc = accuracy.accuracy(y, np.argmax(t, axis=1))
            reporter.report({'accuracy': acc}, self)
        return loss


class MyIterator(chainer.iterators.SerialIterator):
    """Preprocesses labels to onehot vectors."""

    def __next__(self):
        batch = []
        for input, label in super(MyIterator, self).__next__():
            onehot = np.eye(10, dtype=input.dtype)[label]
            batch.append((input, onehot))
        return batch

    def next(self):
        return self.__next__()


# Network definition
class MLP(chainer.Chain):

    def __init__(self, n_units, n_out, use_sigmoid=False):
        super(MLP, self).__init__()
        self.activation_fn = F.sigmoid if use_sigmoid else F.relu
        with self.init_scope():
            # the size of the inputs to each layer will be inferred
            self.l1 = L.Linear(None, n_units)  # n_in -> n_units
            self.l2 = L.Linear(None, n_units)  # n_units -> n_units
            self.l3 = L.Linear(None, n_out)  # n_units -> n_out

    def __call__(self, x):
        h1 = self.activation_fn(self.l1(x))
        h2 = self.activation_fn(self.l2(h1))
        return self.l3(h2)


def main():
    parser = argparse.ArgumentParser(description='Chainer example: MNIST')
    parser.add_argument('--batchsize', '-b', type=int, default=7,
                        help='Number of images in each mini-batch')
    parser.add_argument('--epoch', '-e', type=int, default=20,
                        help='Number of sweeps over the dataset to train')
    parser.add_argument('--frequency', '-f', type=int, default=-1,
                        help='Frequency of taking a snapshot')
    parser.add_argument('--gpu', '-g', type=int, default=-1,
                        help='GPU ID (negative value indicates CPU)')
    parser.add_argument('--out', '-o', default='result',
                        help='Directory to output the result')
    parser.add_argument('--resume', '-r', default='',
                        help='Resume the training from snapshot')
    parser.add_argument('--unit', '-u', type=int, default=1000,
                        help='Number of units')
    parser.add_argument('--noplot', dest='plot', action='store_false',
                        help='Disable PlotReport extension')
    parser.add_argument('--onnx', default='',
                        help='Export ONNX model')
    parser.add_argument('--model', '-m', default='model.npz',
                        help='Model file name to serialize')
    parser.add_argument('--timeout', type=int, default=0,
                        help='Enable timeout')
    parser.add_argument('--trace', default='',
                        help='Enable tracing')
    parser.add_argument('--train', action='store_true',
                        help='Run training')
    args = parser.parse_args()

    main_impl(args)


def main_impl(args):
    # Set up a neural network to train
    # Classifier reports softmax cross entropy loss and accuracy at every
    # iteration, which will be used by the PrintReport extension below.
    model = MLP(args.unit, 10)
    # classifier = L.Classifier(model)
    classifier = MyClassifier(model, compute_accuracy=args.train)

    model = classifier

    replace_id(model)

    if args.gpu >= 0:
        # Make a specified GPU current
        chainer.backends.cuda.get_device_from_id(args.gpu).use()
        model.to_gpu()  # Copy the model to the GPU

    # Setup an optimizer
    #optimizer = chainer.optimizers.Adam()
    optimizer = chainer.optimizers.SGD()
    optimizer.setup(model)

    # Load the MNIST dataset
    train, test = chainer.datasets.get_mnist()

    train_iter = MyIterator(train, args.batchsize, shuffle=False)
    test_iter = MyIterator(test, args.batchsize, repeat=False, shuffle=False)

    # Set up a trainer
    updater = training.updaters.StandardUpdater(
        train_iter, optimizer, device=args.gpu)
    trainer = training.Trainer(updater, (args.epoch, 'epoch'), out=args.out)

    if args.train:
        # Evaluate the model with the test dataset for each epoch
        trainer.extend(extensions.Evaluator(test_iter, model, device=args.gpu))
        run_training(args, trainer)
        return

    out_dir = 'out/backprop_test_mnist_mlp'
    makedirs(out_dir)

    for step in range(2):
        trainer.updater.update()
        npz_filename = '%s/params_%d.npz' % (out_dir, step)
        params_dir = '%s/params_%d' % (out_dir, step)
        chainer.serializers.save_npz(npz_filename, model)
        makedirs(params_dir)
        npz_to_onnx.npz_to_onnx(npz_filename, os.path.join(params_dir, 'param'))

    chainer.config.train = False
    x = np.random.random((args.batchsize, 784)).astype(np.float32)
    y = (np.random.random(args.batchsize) * 10).astype(np.int32)
    onehot = np.eye(10, dtype=x.dtype)[y]
    x = chainer.Variable(x, name='input')
    onehot = chainer.Variable(onehot, name='onehot')
    onnx_chainer.export(model, (x, onehot),
                        filename='%s/model.onnx' % out_dir)

    test_data_dir = '%s/test_data_set_0' % out_dir
    makedirs(test_data_dir)
    for i, var in enumerate([x, onehot, model.batch_size]):
        with open(os.path.join(test_data_dir, 'input_%d.pb' % i), 'wb') as f:
            t = npz_to_onnx.np_array_to_onnx(var.name, var.data)
            f.write(t.SerializeToString())

    model.cleargrads()
    result = model(x, onehot)
    result.backward()
    for i, (name, param) in enumerate(model.namedparams()):
        with open(os.path.join(test_data_dir, 'output_%d.pb' % i), 'wb') as f:
            t = npz_to_onnx.np_array_to_onnx('grad_out@' + name, param.grad)
            f.write(t.SerializeToString())


def run_training(args, trainer):
    # Dump a computational graph from 'loss' variable at the first iteration
    # The "main" refers to the target link of the "main" optimizer.
    trainer.extend(extensions.dump_graph('main/loss'))

    # Take a snapshot for each specified epoch
    frequency = args.epoch if args.frequency == -1 else max(1, args.frequency)
    trainer.extend(extensions.snapshot(), trigger=(frequency, 'epoch'))

    # Write a log of evaluation statistics for each epoch
    trainer.extend(extensions.LogReport())

    # Save two plot images to the result dir
    if args.plot and extensions.PlotReport.available():
        trainer.extend(
            extensions.PlotReport(['main/loss', 'validation/main/loss'],
                                  'epoch', file_name='loss.png'))
        trainer.extend(
            extensions.PlotReport(
                ['main/accuracy', 'validation/main/accuracy'],
                'epoch', file_name='accuracy.png'))

    # Print selected entries of the log to stdout
    # Here "main" refers to the target link of the "main" optimizer again, and
    # "validation" refers to the default name of the Evaluator extension.
    # Entries other than 'epoch' are reported by the Classifier link, called by
    # either the updater or the evaluator.
    trainer.extend(extensions.PrintReport(
        ['epoch', 'main/loss', 'validation/main/loss',
         'main/accuracy', 'validation/main/accuracy', 'elapsed_time']))

    # Print a progress bar to stdout
    trainer.extend(extensions.ProgressBar())

    if args.resume:
        # Resume from a snapshot
        chainer.serializers.load_npz(args.resume, trainer)

    # Run the training
    trainer.run()


if __name__ == '__main__':
    main()
