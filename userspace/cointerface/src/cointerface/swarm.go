package main

import (
	"context"
	"fmt"

	"draiosproto"
	"sdc_internal"
	log "github.com/cihub/seelog"
	"github.com/docker/docker/api/types"
	"github.com/docker/docker/api/types/filters"
	"github.com/docker/docker/api/types/swarm"
	"github.com/docker/docker/client"
	"github.com/gogo/protobuf/proto"
	"os"
	"strings"
)

func labelsToProtobuf(labels map[string]string) (ret []*draiosproto.SwarmPair) {
	for k, v := range labels {
		ret = append(ret, &draiosproto.SwarmPair{Key: proto.String(k), Value: proto.String(v)})
	}
	return
}

func virtualIPsToProtobuf(vIPs []swarm.EndpointVirtualIP) (ret []string) {
	for _, vip := range vIPs {
		ret = append(ret, vip.Addr)
	}
	return
}

func portsToProtobuf(ports []swarm.PortConfig) (ret []*draiosproto.SwarmPort) {
	for _, port := range ports {
		ret = append(ret, &draiosproto.SwarmPort{Port: proto.Uint32(port.TargetPort),
			PublishedPort: proto.Uint32(port.PublishedPort),
			Protocol:      proto.String(string(port.Protocol))})
	}
	return
}

func serviceToProtobuf(service swarm.Service) *draiosproto.SwarmService {
	return &draiosproto.SwarmService{Common: &draiosproto.SwarmCommon{
			Id:     proto.String(service.ID),
			Name:   proto.String(service.Spec.Name),
			Labels: labelsToProtobuf(service.Spec.Labels)},
		VirtualIps: virtualIPsToProtobuf(service.Endpoint.VirtualIPs),
		Ports:      portsToProtobuf(service.Endpoint.Ports),
	}
}

func taskToProtobuf(task swarm.Task) *draiosproto.SwarmTask {
	cidlen := len(task.Status.ContainerStatus.ContainerID)
	if cidlen > 12 {
		cidlen = 12
	}
	return &draiosproto.SwarmTask{Common: &draiosproto.SwarmCommon{
			Id: proto.String(task.ID),
		},
		ServiceId:   proto.String(task.ServiceID),
		NodeId:      proto.String(task.NodeID),
		ContainerId: proto.String(task.Status.ContainerStatus.ContainerID[:cidlen])}
}

func nodeToProtobuf(node swarm.Node) *draiosproto.SwarmNode {
	var addr string
	// It looks that sometimes node.Status.Addr is 127.0.0.1
	// on managers, so for them report the ManagerStatus.Addr
	// docker issue: https://github.com/docker/docker/issues/30119
	if node.ManagerStatus != nil {
		addr = strings.Split(node.ManagerStatus.Addr, ":")[0]
	} else {
		addr = node.Status.Addr
	}
	sn := draiosproto.SwarmNode{
		Common: &draiosproto.SwarmCommon{
			Id:     proto.String(node.ID),
			Name:   proto.String(node.Description.Hostname),
			Labels: labelsToProtobuf(node.Spec.Labels),
		},
		Role: proto.String(string(node.Spec.Role)),
		IpAddress: proto.String(addr),
		Version: proto.String(node.Description.Engine.EngineVersion),
		Availability: proto.String(string(node.Spec.Availability)),
		State: proto.String(string(node.Status.State))}
	if node.ManagerStatus != nil {
		sn.Manager = &draiosproto.SwarmManager{
			Leader: proto.Bool(node.ManagerStatus.Leader),
			Reachability: proto.String(string(node.ManagerStatus.Reachability))}
	}
	return &sn
}

func quorum(nodes []swarm.Node) (*bool) {
	var on, total uint32 = 0, 0
	for _, node := range nodes {
		if node.ManagerStatus != nil {
			if node.ManagerStatus.Reachability == swarm.ReachabilityReachable {
				on++
			}
			total++
		}
	}
	var q bool = on >= (total / 2) + 1
	return &q
}

func getSwarmState(ctx context.Context, cmd *sdc_internal.SwarmStateCommand) (*sdc_internal.SwarmStateResult, error) {
	log.Debugf("Received swarmstate command message: %s", cmd.String())

	// If SYSDIG_HOST_ROOT is set, use that as a part of the
	// socket path.

	sysdigRoot := os.Getenv("SYSDIG_HOST_ROOT")
	if sysdigRoot != "" {
		sysdigRoot = sysdigRoot + "/"
	}
	dockerSock := fmt.Sprintf("unix:///%svar/run/docker.sock", sysdigRoot)
	cli, err := client.NewClient(dockerSock, "v1.26", nil, nil)
	if err != nil {
		ferr := log.Errorf("Could not create docker client: %s", err)
		log.Errorf(ferr.Error())
		return nil, ferr
	}

	info, err := cli.Info(context.Background())
	if err != nil {
		ferr := log.Errorf("Could not get docker client info: %s", err)
		return nil, ferr
	}
	clusterId := proto.String(info.Swarm.Cluster.ID)
	isManager := info.Swarm.ControlAvailable

	m := &draiosproto.SwarmState{ClusterId: clusterId}

	if isManager {
		if services, err := cli.ServiceList(ctx, types.ServiceListOptions{}); err == nil {
			for _, service := range services {
				m.Services = append(m.Services, serviceToProtobuf(service))
				stack := service.Spec.Labels["com.docker.stack.namespace"]
				if stack == "" {
					stack = "none"
				}
				// fmt.Printf("service id=%s name=%s stack=%s ip=%s\n", service.ID[:10], service.Spec.Name, stack, virtualIPsToProtobuf(service.Endpoint.VirtualIPs))
			}
		} else {
			log.Errorf("Error fetching services: %s\n", err)
		}

		if nodes, err := cli.NodeList(ctx, types.NodeListOptions{}); err == nil {
			for _, node := range nodes {
				m.Nodes = append(m.Nodes, nodeToProtobuf(node))
				// fmt.Printf("node id=%s name=%s role=%s availability=%s\n", node.ID, node.Description.Hostname, node.Spec.Role, node.Spec.Availability)
			}
			m.Quorum = quorum(nodes)
		} else {
			log.Errorf("Error fetching nodes: %s\n", err)
		}

		args := filters.NewArgs()
		args.Add("desired-state", "running")
		args.Add("desired-state", "accepted")
		if tasks, err := cli.TaskList(ctx, types.TaskListOptions{Filters: args}); err == nil {
			for _, task := range tasks {
				m.Tasks = append(m.Tasks, taskToProtobuf(task))
				// fmt.Printf("task id=%s name=%s service=%s node=%s status=%s containerid=%s\n", task.ID, task.Name, task.ServiceID, task.NodeID, task.Status.State, task.Status.ContainerStatus.ContainerID[:12])
			}
		} else {
			log.Errorf("Error fetching tasks: %s\n", err)
		}
	}

    res := &sdc_internal.SwarmStateResult{}
    res.Successful = proto.Bool(err == nil)
    if err != nil {
        res.Errstr = proto.String(err.Error())
    }
	res.State = m

    log.Debugf("SwarmState Sending response: %s", res.String())

    return res, nil
}
