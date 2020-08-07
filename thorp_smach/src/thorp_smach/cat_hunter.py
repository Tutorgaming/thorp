#!/usr/bin/env python

import rospy
import smach
import smach_ros

import toolkit.config as cfg

from explore_house import explore_house_sm
from toolkit.comon_states import *
from toolkit.navigation_states import *
from toolkit.exploration_states import *
from toolkit.cannon_ctrl_states import *


class Attack(FollowPose):
    def __init__(self):
        super(Attack, self).__init__()
        self.last_aim_command_time = 0.0
        self.last_fire_command_time = 0.0
        self.fire_max_dist = rospy.get_param('~fire_max_dist', 1.0)
        self.fire_max_angle = rospy.get_param('~fire_max_angle', 0.2)  # ~12 degrees, should decrease with the distance
        self.follow_distance = self.fire_max_dist - 0.2  # follow a bit beyond the fire distance

        # cannon commands service client
        rospy.wait_for_service('cannon_command', 30.0)
        self.cannon_srv = rospy.ServiceProxy('cannon_command', thorp_srvs.CannonCmd)

    def _goal_feedback_cb(self, feedback):
        super(Attack, self)._goal_feedback_cb(feedback)
        try:
            if rospy.get_time() - self.last_aim_command_time > 1.0 and \
                 feedback.dist_to_target <= self.fire_max_dist * 1.5 and \
                 abs(feedback.angle_to_target) <= self.fire_max_angle * 2.5:
                # do not spam the cannon with aim commands it cannot execute fast
                resp = self.cannon_srv(thorp_srvs.CannonCmdRequest.AIM, None, None)
                if resp.error.code != thorp_msgs.ThorpError.SUCCESS:
                    rospy.logerr("Aim cannon failed with error code %d: %s", resp.error.code, resp.error.text)
                print "AIM CMD"
                self.last_aim_command_time = rospy.get_time()
            if rospy.get_time() - self.last_fire_command_time > 1.0 and \
                 feedback.dist_to_target <= self.fire_max_dist and \
                 abs(feedback.angle_to_target) <= self.fire_max_angle:
                resp = self.cannon_srv(thorp_srvs.CannonCmdRequest.FIRE, None, 1)
                if resp.error.code != thorp_msgs.ThorpError.SUCCESS:
                    rospy.logerr("Fire cannon failed with error code %d: %s", resp.error.code, resp.error.text)
                self.request_preempt()  # TODO: we cannot verify the kill, so by now just stop the attack
                rospy.loginfo("Fired at target at %g m and %g rad!", feedback.dist_to_target, feedback.angle_to_target)
                self.last_fire_command_time = rospy.get_time()
        except rospy.ServiceException as err:
            rospy.logerr("Cannon commands service call failed: %s", err)


def cat_hunter_sm():
    """
    Cat hunter SM:
     - explore house with object recognition enabled
     - trigger hunting upon detecting a cat
    """

    # gets called when ANY child state terminates
    def child_term_cb(outcome_map):

        # terminate all running states if FOO finished with outcome 'outcome3'
        if outcome_map['EXPLORE_HOUSE']:
            return True

        # terminate all running states if BAR finished
        if outcome_map['LOOK_FOR_CATS']:
            return True

        # in all other case, just keep running, don't terminate anything
        return False

    # gets called when ALL child states are terminated
    def out_cb(outcome_map):
        if outcome_map['LOOK_FOR_CATS'] == 'detected':
            return 'detected'
        if outcome_map['EXPLORE_HOUSE'] == 'succeeded':
            return 'not_detected'
        return outcome_map['EXPLORE_HOUSE']

    # creating the concurrence state machine
    search_sm = smach.Concurrence(outcomes=['detected', 'not_detected', 'aborted', 'preempted'],
                                  default_outcome='not_detected',
                                  output_keys=['tracked_object_pose'],
                                  child_termination_cb=child_term_cb,
                                  outcome_cb=out_cb)

    def my_cb(ud):
        rospy.sleep(5)  # TODO ojo,,, no cancelable!!!
        return 'succeeded'
    with search_sm:
#        smach.Concurrence.add('EXPLORE_HOUSE', explore_house_sm())
        smach.Concurrence.add('EXPLORE_HOUSE', smach.CBState(my_cb, outcomes=['succeeded']))
        smach.Concurrence.add('LOOK_FOR_CATS', MonitorObjects(['cat', 'dog', 'horse']))

    # hunt sm
    hunt_sm = smach.Sequence(outcomes=['succeeded', 'aborted', 'preempted'],
                             connector_outcome='succeeded',
                             input_keys=['tracked_object_pose'])
    with hunt_sm:
        smach.Sequence.add('POSE_AS_PATH', PoseAsPath(),
                           remapping={'pose': 'tracked_object_pose'})
        smach.Sequence.add('APPROACH', ExePath(cfg.FOLLOW_CONTROLLER,
                                               1, #FIRE_DISTANCE,
                                               cfg.INF_ANGLE_TOLERANCE))
        smach.Sequence.add('AIM_CANNON', AimCannon())
        smach.Sequence.add('FIRE_CANNON', FireCannon(1))

    # Full SM: plan rooms visit sequence and explore each room in turn
    sm = smach.StateMachine(outcomes=['detected',
                                      'not_detected',
                                      'aborted',
                                      'preempted'])
    with sm:
        smach.StateMachine.add('SEARCH', search_sm,
                               transitions={'detected': 'ATTACK',
                                            'not_detected': 'not_detected',
                                            'aborted': 'aborted',
                                            'preempted': 'preempted'})
        smach.StateMachine.add('ATTACK', Attack(),
                               transitions={'succeeded': 'SEARCH',
                                            'aborted': 'SEARCH',
                                            'preempted': 'SEARCH'})
    return sm


if __name__ == '__main__':
    rospy.init_node('smach_cat_hunter')

    TF2()  # start listener asap

    sm = cat_hunter_sm()

    # Create and start the introspection server
    sis = smach_ros.IntrospectionServer('server_name', sm, '/SM_ROOT')
    sis.start()

    # Execute the state machine
    t0 = rospy.get_time()
    outcome = sm.execute()
    rospy.loginfo("Hunt completed in %.2fs with outcome '%s'", rospy.get_time() - t0, outcome)

    # Wait for ctrl-c to stop the application
    rospy.spin()
    sis.stop()

    rospy.signal_shutdown('All done.')